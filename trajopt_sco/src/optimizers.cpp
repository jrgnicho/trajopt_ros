#include <trajopt_utils/macros.h>
TRAJOPT_IGNORE_WARNINGS_PUSH
#include <boost/format.hpp>
#include <cmath>
#include <cstdio>
#include <stdio.h>
TRAJOPT_IGNORE_WARNINGS_POP

#include <trajopt_sco/expr_ops.hpp>
#include <trajopt_sco/modeling.hpp>
#include <trajopt_sco/optimizers.hpp>
#include <trajopt_sco/sco_common.hpp>
#include <trajopt_sco/solver_interface.hpp>
#include <trajopt_utils/logging.hpp>
#include <trajopt_utils/macros.h>
#include <trajopt_utils/stl_to_string.hpp>

namespace sco
{
std::ostream& operator<<(std::ostream& o, const OptResults& r)
{
  o << "Optimization results:" << std::endl
    << "status: " << statusToString(r.status) << std::endl
    << "cost values: " << util::Str(r.cost_vals) << std::endl
    << "constraint violations: " << util::Str(r.cnt_viols) << std::endl
    << "n func evals: " << r.n_func_evals << std::endl
    << "n qp solves: " << r.n_qp_solves << std::endl;
  return o;
}

//////////////////////////////////////////////////
////////// private utility functions for  sqp /////////
//////////////////////////////////////////////////

static DblVec evaluateCosts(const std::vector<Cost::Ptr>& costs, const DblVec& x)
{
  DblVec out(costs.size());
  for (size_t i = 0; i < costs.size(); ++i)
  {
    out[i] = costs[i]->value(x);
  }
  return out;
}
static DblVec evaluateConstraintViols(const std::vector<Constraint::Ptr>& constraints, const DblVec& x)
{
  DblVec out(constraints.size());
  for (size_t i = 0; i < constraints.size(); ++i)
  {
    out[i] = constraints[i]->violation(x);
  }
  return out;
}
static std::vector<ConvexObjective::Ptr> convexifyCosts(const std::vector<Cost::Ptr>& costs,
                                                        const DblVec& x,
                                                        Model* model)
{
  std::vector<ConvexObjective::Ptr> out(costs.size());
  for (size_t i = 0; i < costs.size(); ++i)
  {
    out[i] = costs[i]->convex(x, model);
  }
  return out;
}
static std::vector<ConvexConstraints::Ptr> convexifyConstraints(const std::vector<Constraint::Ptr>& cnts,
                                                                const DblVec& x,
                                                                Model* model)
{
  std::vector<ConvexConstraints::Ptr> out(cnts.size());
  for (size_t i = 0; i < cnts.size(); ++i)
  {
    out[i] = cnts[i]->convex(x, model);
  }
  return out;
}

DblVec evaluateModelCosts(const std::vector<ConvexObjective::Ptr>& costs, const DblVec& x)
{
  DblVec out(costs.size());
  for (size_t i = 0; i < costs.size(); ++i)
  {
    out[i] = costs[i]->value(x);
  }
  return out;
}
DblVec evaluateModelCntViols(const std::vector<ConvexConstraints::Ptr>& cnts, const DblVec& x)
{
  DblVec out(cnts.size());
  for (size_t i = 0; i < cnts.size(); ++i)
  {
    out[i] = cnts[i]->violation(x);
  }
  return out;
}

static std::vector<std::string> getCostNames(const std::vector<Cost::Ptr>& costs)
{
  std::vector<std::string> out(costs.size());
  for (size_t i = 0; i < costs.size(); ++i)
    out[i] = costs[i]->name();
  return out;
}
static std::vector<std::string> getCntNames(const std::vector<Constraint::Ptr>& cnts)
{
  std::vector<std::string> out(cnts.size());
  for (size_t i = 0; i < cnts.size(); ++i)
    out[i] = cnts[i]->name();
  return out;
}

static std::vector<std::string> getVarNames(const VarVector& vars)
{
  std::vector<std::string> out;
  out.reserve(vars.size());
  for (const auto& var : vars)
    out.push_back(var.var_rep->name);
  return out;
}

// todo: use different coeffs for each constraint
std::vector<ConvexObjective::Ptr> cntsToCosts(const std::vector<ConvexConstraints::Ptr>& cnts,
                                              double err_coeff,
                                              Model* model)
{
  std::vector<ConvexObjective::Ptr> out;
  for (const ConvexConstraints::Ptr& cnt : cnts)
  {
    ConvexObjective::Ptr obj(new ConvexObjective(model));
    for (const AffExpr& aff : cnt->eqs_)
    {
      obj->addAbs(aff, err_coeff);
    }
    for (const AffExpr& aff : cnt->ineqs_)
    {
      obj->addHinge(aff, err_coeff);
    }
    out.push_back(obj);
  }
  return out;
}

void Optimizer::addCallback(const Callback& cb) { callbacks_.push_back(cb); }
void Optimizer::callCallbacks()
{
  for (unsigned i = 0; i < callbacks_.size(); ++i)
  {
    callbacks_[i](prob_.get(), results_);
  }
}

void Optimizer::initialize(const DblVec& x)
{
  if (!prob_)
    PRINT_AND_THROW("need to set the problem before initializing");
  if (prob_->getVars().size() != x.size())
    PRINT_AND_THROW(boost::format("initialization vector has wrong length. expected %i got %i") %
                    prob_->getVars().size() % x.size());
  results_.clear();
  results_.x = x;
}

BasicTrustRegionSQPParameters::BasicTrustRegionSQPParameters()
{
  improve_ratio_threshold = 0.25;
  min_trust_box_size = 1e-4;
  min_approx_improve = 1e-4;
  min_approx_improve_frac = static_cast<double>(-INFINITY);
  max_iter = 50;
  trust_shrink_ratio = 0.1;
  trust_expand_ratio = 1.5;
  cnt_tolerance = 1e-4;
  max_merit_coeff_increases = 5;
  merit_coeff_increase_ratio = 10;
  max_time = static_cast<double>(INFINITY);
  merit_error_coeff = 10;
  trust_box_size = 1e-1;
  log_results = false;
  log_dir = "/tmp";
}

BasicTrustRegionSQP::BasicTrustRegionSQP() {}
BasicTrustRegionSQP::BasicTrustRegionSQP(OptProb::Ptr prob) { setProblem(prob); }
void BasicTrustRegionSQP::setProblem(OptProb::Ptr prob)
{
  Optimizer::setProblem(prob);
  model_ = prob->getModel();
}

void BasicTrustRegionSQP::adjustTrustRegion(double ratio) { param_.trust_box_size *= ratio; }
void BasicTrustRegionSQP::setTrustBoxConstraints(const DblVec& x)
{
  const VarVector& vars = prob_->getVars();
  assert(vars.size() == x.size());
  const DblVec &lb = prob_->getLowerBounds(), ub = prob_->getUpperBounds();
  DblVec lbtrust(x.size()), ubtrust(x.size());
  for (size_t i = 0; i < x.size(); ++i)
  {
    lbtrust[i] = fmax(x[i] - param_.trust_box_size, lb[i]);
    ubtrust[i] = fmin(x[i] + param_.trust_box_size, ub[i]);
  }
  model_->setVarBounds(vars, lbtrust, ubtrust);
}

#if 0
struct MultiCritFilter {
  /**
   * Checks if you're making an improvement on a multidimensional objective
   * Given a set of past error vectors, the improvement is defined as
   * min_{olderrvec in past_err_vecs} | olderrvec - errvec |^+
   */
  vector<DblVec> errvecs;
  double improvement(const DblVec& errvec) {
    double leastImprovement=INFINITY;
    for (const DblVec& olderrvec : errvecs) {
      double improvement=0;
      for (int i=0; i < errvec.size(); ++i) improvement += pospart(olderrvec[i] - errvec[i]);
      leastImprovement = fmin(leastImprovement, improvement);
    }
    return leastImprovement;
  }
  void insert(const DblVec& x) {errvecs.push_back(x);}
  bool empty() {return errvecs.size() > 0;}
};
#endif

BasicTrustRegionSQPResults::BasicTrustRegionSQPResults(const std::vector<std::string>& var_names,
                                                       const std::vector<std::string>& cost_names,
                                                       const std::vector<std::string>& cnt_names)
  : var_names(var_names), cost_names(cost_names), cnt_names(cnt_names)
{
  model_var_vals.clear();
  model_cost_vals.clear();
  model_cnt_viols.clear();
  new_x.clear();
  new_cost_vals.clear();
  old_cost_vals.clear();
  new_cnt_viols.clear();
  old_cnt_viols.clear();
  old_merit = 0;
  model_merit = 0;
  new_merit = 0;
  approx_merit_improve = 0;
  exact_merit_improve = 0;
  merit_improve_ratio = 0;
  merit_error_coeff = 0;
}

void BasicTrustRegionSQPResults::update(const OptResults& prev_opt_results,
                                        const Model& model,
                                        const std::vector<ConvexObjective::Ptr>& cost_models,
                                        const std::vector<ConvexConstraints::Ptr>& cnt_models,
                                        const std::vector<ConvexObjective::Ptr>& cnt_cost_models,
                                        const std::vector<Constraint::Ptr>& constraints,
                                        const std::vector<Cost::Ptr>& costs,
                                        const double merit_error_coeff)
{
  this->merit_error_coeff = merit_error_coeff;
  model_var_vals = model.getVarValues(model.getVars());
  model_cost_vals = evaluateModelCosts(cost_models, model_var_vals);
  model_cnt_viols = evaluateModelCntViols(cnt_models, model_var_vals);

  // the n variables of the OptProb happen to be the first n variables in
  // the Model
  new_x = DblVec(model_var_vals.begin(), model_var_vals.begin() + static_cast<long int>(prev_opt_results.x.size()));

  if (util::GetLogLevel() >= util::LevelDebug)
  {
    DblVec cnt_costs1 = evaluateModelCosts(cnt_cost_models, model_var_vals);
    DblVec cnt_costs2 = model_cnt_viols;
    for (unsigned i = 0; i < cnt_costs2.size(); ++i)
      cnt_costs2[i] *= merit_error_coeff;
    LOG_DEBUG("SHOULD BE ALMOST THE SAME: %s ?= %s", CSTR(cnt_costs1), CSTR(cnt_costs2));
    // not exactly the same because cnt_costs1 is based on aux variables,
    // but they might not be at EXACTLY the right value
  }

  old_cost_vals = prev_opt_results.cost_vals;
  old_cnt_viols = prev_opt_results.cnt_viols;
  new_cost_vals = evaluateCosts(costs, new_x);
  new_cnt_viols = evaluateConstraintViols(constraints, new_x);

  old_merit = vecSum(old_cost_vals) + merit_error_coeff * vecSum(old_cnt_viols);
  model_merit = vecSum(model_cost_vals) + merit_error_coeff * vecSum(model_cnt_viols);
  new_merit = vecSum(new_cost_vals) + merit_error_coeff * vecSum(new_cnt_viols);
  approx_merit_improve = old_merit - model_merit;
  exact_merit_improve = old_merit - new_merit;
  merit_improve_ratio = exact_merit_improve / approx_merit_improve;

  if (util::GetLogLevel() >= util::LevelInfo)
  {
    LOG_INFO(" ");
    print();
  }
}

void BasicTrustRegionSQPResults::print() const
{
  std::printf("%15s | %10s | %10s | %10s | %10s\n", "", "oldexact", "dapprox", "dexact", "ratio");
  std::printf("%15s | %10s---%10s---%10s---%10s\n", "COSTS", "----------", "----------", "----------", "----------");
  for (size_t i = 0; i < old_cost_vals.size(); ++i)
  {
    double approx_improve = old_cost_vals[i] - model_cost_vals[i];
    double exact_improve = old_cost_vals[i] - new_cost_vals[i];
    if (fabs(approx_improve) > 1e-8)
      std::printf("%15s | %10.3e | %10.3e | %10.3e | %10.3e\n",
                  cost_names[i].c_str(),
                  old_cost_vals[i],
                  approx_improve,
                  exact_improve,
                  exact_improve / approx_improve);
    else
      std::printf("%15s | %10.3e | %10.3e | %10.3e | %10s\n",
                  cost_names[i].c_str(),
                  old_cost_vals[i],
                  approx_improve,
                  exact_improve,
                  "  ------  ");
  }

  if (cnt_names.size() != 0)
  {
    std::printf("%15s | %10s---%10s---%10s---%10s\n",
                "CONSTRAINTS",
                "----------",
                "----------",
                "----------",
                "---------"
                "-");
    for (size_t i = 0; i < old_cnt_viols.size(); ++i)
    {
      double approx_improve = old_cnt_viols[i] - model_cnt_viols[i];
      double exact_improve = old_cnt_viols[i] - new_cnt_viols[i];
      if (fabs(approx_improve) > 1e-8)
        std::printf("%15s | %10.3e | %10.3e | %10.3e | %10.3e\n",
                    cnt_names[i].c_str(),
                    merit_error_coeff * old_cnt_viols[i],
                    merit_error_coeff * approx_improve,
                    merit_error_coeff * exact_improve,
                    exact_improve / approx_improve);
      else
        std::printf("%15s | %10.3e | %10.3e | %10.3e | %10s\n",
                    cnt_names[i].c_str(),
                    merit_error_coeff * old_cnt_viols[i],
                    merit_error_coeff * approx_improve,
                    merit_error_coeff * exact_improve,
                    "  ------  ");
    }
  }

  std::printf("%15s | %10.3e | %10.3e | %10.3e | %10.3e\n",
              "TOTAL",
              old_merit,
              approx_merit_improve,
              exact_merit_improve,
              merit_improve_ratio);
}

void BasicTrustRegionSQPResults::writeSolver(std::FILE* stream, bool header) const
{
  if (header)
    std::fprintf(stream, "%s,%s,%s,%s,%s\n", "DESCRIPTION", "oldexact", "dapprox", "dexact", "ratio");

  std::fprintf(stream,
               "%s,%10.3e,%10.3e,%10.3e,%10.3e\n",
               "Solver",
               old_merit,
               approx_merit_improve,
               exact_merit_improve,
               merit_improve_ratio);
  std::fflush(stream);
}

void BasicTrustRegionSQPResults::writeVars(std::FILE* stream, bool header) const
{
  if (header)
  {
    std::fprintf(stream, "%s", "NAMES");
    for (const auto& var : var_names)
      std::fprintf(stream, ",%s", var.c_str());

    std::fprintf(stream, "\n");
  }

  std::fprintf(stream, "%s", "VALUES");
  for (const auto& x : new_x)
    std::fprintf(stream, ",%e", x);

  std::fprintf(stream, "\n");
  std::fflush(stream);
}

void BasicTrustRegionSQPResults::writeCosts(std::FILE* stream, bool header) const
{
  if (header)
  {
    std::fprintf(stream, "%s", "COST NAMES");
    for (const auto& name : cost_names)
      std::fprintf(stream, ",%s,%s,%s,%s", name.c_str(), name.c_str(), name.c_str(), name.c_str());

    std::fprintf(stream, "\n");

    std::fprintf(stream, "%s", "DESCRIPTION");
    for (size_t i = 0; i < cost_names.size(); ++i)
      std::fprintf(stream, ",%s,%s,%s,%s", "oldexact", "dapprox", "dexact", "ratio");

    std::fprintf(stream, "\n");
  }

  std::fprintf(stream, "%s", "COSTS");
  for (size_t i = 0; i < old_cost_vals.size(); ++i)
  {
    double approx_improve = old_cost_vals[i] - model_cost_vals[i];
    double exact_improve = old_cost_vals[i] - new_cost_vals[i];
    if (fabs(approx_improve) > 1e-8)
    {
      std::fprintf(
          stream, ",%e,%e,%e,%e", old_cost_vals[i], approx_improve, exact_improve, exact_improve / approx_improve);
    }
    else
    {
      std::fprintf(stream, ",%e,%e,%e,%s", old_cost_vals[i], approx_improve, exact_improve, "nan");
    }
  }
  std::fprintf(stream, "\n");
  std::fflush(stream);
}

void BasicTrustRegionSQPResults::writeConstraints(std::FILE* stream, bool header) const
{
  if (header)
  {
    std::fprintf(stream, "%s", "CONSTRAINT NAMES");
    for (const auto& name : cnt_names)
      std::fprintf(stream, ",%s,%s,%s,%s", name.c_str(), name.c_str(), name.c_str(), name.c_str());

    std::fprintf(stream, "\n");

    std::fprintf(stream, "%s", "DESCRIPTION");
    for (size_t i = 0; i < cnt_names.size(); ++i)
      std::fprintf(stream, ",%s,%s,%s,%s", "oldexact", "dapprox", "dexact", "ratio");

    std::fprintf(stream, "\n");
  }

  std::fprintf(stream, "%s", "CONSTRAINTS");
  for (size_t i = 0; i < old_cnt_viols.size(); ++i)
  {
    double approx_improve = old_cnt_viols[i] - model_cnt_viols[i];
    double exact_improve = old_cnt_viols[i] - new_cnt_viols[i];
    if (fabs(approx_improve) > 1e-8)
    {
      std::fprintf(stream,
                   ",%e,%e,%e,%e",
                   merit_error_coeff * old_cnt_viols[i],
                   merit_error_coeff * approx_improve,
                   merit_error_coeff * exact_improve,
                   exact_improve / approx_improve);
    }
    else
    {
      std::fprintf(stream,
                   ",%e,%e,%e,%s",
                   merit_error_coeff * old_cnt_viols[i],
                   merit_error_coeff * approx_improve,
                   merit_error_coeff * exact_improve,
                   "nan");
    }
  }
  std::fprintf(stream, "\n");
  std::fflush(stream);
}

OptStatus BasicTrustRegionSQP::optimize()
{
  std::vector<std::string> var_names = getVarNames(prob_->getVars());
  std::vector<std::string> cost_names = getCostNames(prob_->getCosts());
  std::vector<Constraint::Ptr> constraints = prob_->getConstraints();
  std::vector<std::string> cnt_names = getCntNames(constraints);
  BasicTrustRegionSQPResults iteration_results(var_names, cost_names, cnt_names);

  std::FILE* log_solver_stream = nullptr;
  std::FILE* log_vars_stream = nullptr;
  std::FILE* log_costs_stream = nullptr;
  std::FILE* log_constraints_stream = nullptr;
  if (param_.log_results || util::GetLogLevel() >= util::LevelDebug)
  {
    log_solver_stream = std::fopen((param_.log_dir + "/trajopt_solver.log").c_str(), "w");
    log_vars_stream = std::fopen((param_.log_dir + "/trajopt_vars.log").c_str(), "w");
    log_costs_stream = std::fopen((param_.log_dir + "/trajopt_costs.log").c_str(), "w");
    log_constraints_stream = std::fopen((param_.log_dir + "/trajopt_constraints.log").c_str(), "w");
  }

  if (results_.x.size() == 0)
    PRINT_AND_THROW("you forgot to initialize!");
  if (!prob_)
    PRINT_AND_THROW("you forgot to set the optimization problem");

  results_.x = prob_->getClosestFeasiblePoint(results_.x);

  assert(results_.x.size() == prob_->getVars().size());
  assert(prob_->getCosts().size() > 0 || constraints.size() > 0);

  OptStatus retval = INVALID;

  for (int merit_increases = 0; merit_increases < param_.max_merit_coeff_increases; ++merit_increases)
  { /* merit adjustment loop */
    for (int iter = 1;; ++iter)
    { /* sqp loop */
      callCallbacks();

      LOG_DEBUG("current iterate: %s", CSTR(results_.x));
      LOG_INFO("iteration %i", iter);

      // speedup: if you just evaluated the cost when doing the line search, use
      // that
      if (results_.cost_vals.empty() && results_.cnt_viols.empty())
      {  // only happens on the first iteration
        results_.cnt_viols = evaluateConstraintViols(constraints, results_.x);
        results_.cost_vals = evaluateCosts(prob_->getCosts(), results_.x);
        assert(results_.n_func_evals == 0);
        ++results_.n_func_evals;
      }

      // DblVec new_cnt_viols = evaluateConstraintViols(constraints, results_.x);
      // DblVec new_cost_vals = evaluateCosts(prob_->getCosts(), results_.x);
      // cout << "costs" << endl;
      // for (int i=0; i < new_cnt_viols.size(); ++i) {
      //   cout << cnt_names[i] << " " << new_cnt_viols[i] -
      //   results_.cnt_viols[i] << endl;
      // }
      // for (int i=0; i < new_cost_vals.size(); ++i) {
      //   cout << cost_names[i] << " " << new_cost_vals[i] -
      //   results_.cost_vals[i] << endl;
      // }

      std::vector<ConvexObjective::Ptr> cost_models = convexifyCosts(prob_->getCosts(), results_.x, model_.get());
      std::vector<ConvexConstraints::Ptr> cnt_models = convexifyConstraints(constraints, results_.x, model_.get());
      std::vector<ConvexObjective::Ptr> cnt_cost_models =
          cntsToCosts(cnt_models, param_.merit_error_coeff, model_.get());
      model_->update();
      for (ConvexObjective::Ptr& cost : cost_models)
        cost->addConstraintsToModel();
      for (ConvexObjective::Ptr& cost : cnt_cost_models)
        cost->addConstraintsToModel();
      model_->update();
      QuadExpr objective;
      for (ConvexObjective::Ptr& co : cost_models)
        exprInc(objective, co->quad_);
      for (ConvexObjective::Ptr& co : cnt_cost_models)
        exprInc(objective, co->quad_);

      //    objective = cleanupExpr(objective);
      model_->setObjective(objective);

      //    if (logging::filter() >= IPI_LEVEL_DEBUG) {
      //      DblVec model_cost_vals;
      //      for (ConvexObjectivePtr& cost : cost_models) {
      //        model_cost_vals.push_back(cost->value(x));
      //      }
      //      LOG_DEBUG("model costs %s should equalcosts  %s",
      //      printer(model_cost_vals), printer(cost_vals));
      //    }

      while (param_.trust_box_size >= param_.min_trust_box_size)
      {
        setTrustBoxConstraints(results_.x);
        CvxOptStatus status = model_->optimize();

        ++results_.n_qp_solves;
        if (status != CVX_SOLVED)
        {
          LOG_ERROR("convex solver failed! set TRAJOPT_LOG_THRESH=DEBUG to see "
                    "solver output. saving model to /tmp/fail.lp and IIS to "
                    "/tmp/fail.ilp");
          model_->writeToFile("/tmp/fail.lp");
          model_->writeToFile("/tmp/fail.ilp");
          retval = OPT_FAILED;
          goto cleanup;
        }

        iteration_results.update(results_,
                                 *model_,
                                 cost_models,
                                 cnt_models,
                                 cnt_cost_models,
                                 constraints,
                                 prob_->getCosts(),
                                 param_.merit_error_coeff);

        if (param_.log_results || util::GetLogLevel() >= util::LevelDebug)
        {
          if (log_solver_stream != nullptr)
            iteration_results.writeSolver(log_solver_stream, results_.n_func_evals == 1);

          if (log_vars_stream != nullptr)
            iteration_results.writeVars(log_vars_stream, results_.n_func_evals == 1);

          if (log_costs_stream != nullptr)
            iteration_results.writeCosts(log_costs_stream, results_.n_func_evals == 1);

          if (log_constraints_stream != nullptr)
            iteration_results.writeConstraints(log_constraints_stream, results_.n_func_evals == 1);
        }

        ++results_.n_func_evals;

        if (iteration_results.approx_merit_improve < -1e-5)
        {
          LOG_ERROR("approximate merit function got worse (%.3e). "
                    "(convexification is probably wrong to zeroth order)",
                    iteration_results.approx_merit_improve);
        }

        if (iteration_results.approx_merit_improve < param_.min_approx_improve)
        {
          LOG_INFO("converged because improvement was small (%.3e < %.3e)",
                   iteration_results.approx_merit_improve,
                   param_.min_approx_improve);
          retval = OPT_CONVERGED;
          goto penaltyadjustment;
        }

        if (iteration_results.approx_merit_improve / iteration_results.old_merit < param_.min_approx_improve_frac)
        {
          LOG_INFO("converged because improvement ratio was small (%.3e < %.3e)",
                   iteration_results.approx_merit_improve / iteration_results.old_merit,
                   param_.min_approx_improve_frac);
          retval = OPT_CONVERGED;
          goto penaltyadjustment;
        }
        else if (iteration_results.exact_merit_improve < 0 ||
                 iteration_results.merit_improve_ratio < param_.improve_ratio_threshold)
        {
          adjustTrustRegion(param_.trust_shrink_ratio);
          LOG_INFO("shrunk trust region. new box size: %.4f", param_.trust_box_size);
        }
        else
        {
          results_.x = iteration_results.new_x;
          results_.cost_vals = iteration_results.new_cost_vals;
          results_.cnt_viols = iteration_results.new_cnt_viols;
          adjustTrustRegion(param_.trust_expand_ratio);
          LOG_INFO("expanded trust region. new box size: %.4f", param_.trust_box_size);
          break;
        }
      }

      if (param_.trust_box_size < param_.min_trust_box_size)
      {
        LOG_INFO("converged because trust region is tiny");
        retval = OPT_CONVERGED;
        goto penaltyadjustment;
      }
      else if (iter >= param_.max_iter)
      {
        LOG_INFO("iteration limit");
        retval = OPT_SCO_ITERATION_LIMIT;
        goto cleanup;
      }
    }

  penaltyadjustment:
    if (results_.cnt_viols.empty() || vecMax(results_.cnt_viols) < param_.cnt_tolerance)
    {
      if (results_.cnt_viols.size() > 0)
        LOG_INFO("woo-hoo! constraints are satisfied (to tolerance %.2e)", param_.cnt_tolerance);
      goto cleanup;
    }
    else
    {
      LOG_INFO("not all constraints are satisfied. increasing penalties");
      param_.merit_error_coeff *= param_.merit_coeff_increase_ratio;
      param_.trust_box_size = fmax(param_.trust_box_size, param_.min_trust_box_size / param_.trust_shrink_ratio * 1.5);
    }
  }
  retval = OPT_PENALTY_ITERATION_LIMIT;
  LOG_INFO("optimization couldn't satisfy all constraints");

cleanup:
  assert(retval != INVALID && "should never happen");
  results_.status = retval;
  results_.total_cost = vecSum(results_.cost_vals);
  LOG_INFO("\n==================\n%s==================", CSTR(results_));
  callCallbacks();

  if (param_.log_results || util::GetLogLevel() >= util::LevelDebug)
  {
    std::fclose(log_solver_stream);
    std::fclose(log_vars_stream);
    std::fclose(log_costs_stream);
    std::fclose(log_constraints_stream);
  }

  return retval;
}
}  // namespace sco
