// (C) Copyright International Business Machines Corporation and Carnegie Mellon University 2006
// All Rights Reserved.
// This code is published under the Common Public License.
//
// Authors :
// Andreas Waechter, International Business Machines Corporation
//                   (derived from BonTMINLP2TNLP.cpp)            12/22/2006
// Authors :


#include "BonBranchingTQP.hpp"
#include "IpBlas.hpp"
#include "IpAlgTypes.hpp"
#include <string>
#include <fstream>
#include <sstream>
namespace Bonmin
{
  BranchingTQP::BranchingTQP(const TMINLP2TNLP& tminlp2tnlp)
    :
    TMINLP2TNLP(tminlp2tnlp)
  {
    DBG_ASSERT(x_sol_);
    DBG_ASSERT(duals_sol_);

    obj_grad_ = new Number[n_];
    obj_hess_ = new Number[nnz_h_lag_];
    obj_hess_irow_ = new Index[nnz_h_lag_];
    obj_hess_jcol_ = new Index[nnz_h_lag_];
    g_vals_ = new Number[m_];
    g_jac_ = new Number[nnz_jac_g_];
    g_jac_irow_ = new Index[nnz_jac_g_];
    g_jac_jcol_ = new Index[nnz_jac_g_];

    // Compute all nonlinear values at the starting point so that we
    // have all the information for the QP
    bool new_x = true;   // ToDo: maybe NOT new?
    bool retval = tminlp_->eval_f(n_, x_sol_, new_x, obj_value_);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate objective function in BranchingTQP");
    new_x = false;
    retval = tminlp_->eval_grad_f(n_, x_sol_, new_x, obj_grad_);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate objective gradient in BranchingTQP");
    bool new_lambda = true; // ToDo: maybe NOT new?
    retval = tminlp_->eval_h(n_, x_sol_, new_x, 1., m_, duals_sol_,
			     new_lambda, nnz_h_lag_, obj_hess_irow_,
			     obj_hess_jcol_, NULL);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate objective Hessian structure in BranchingTQP");
    if (index_style_ == TNLP::FORTRAN_STYLE) {
      for (Index i=0; i<nnz_h_lag_; i++) {
	obj_hess_irow_[i]--;
	obj_hess_jcol_[i]--;
      }
    }
    retval = tminlp_->eval_h(n_, x_sol_, new_x, 1., m_, duals_sol_,
			     new_lambda, nnz_h_lag_, NULL, NULL, obj_hess_);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate objective Hessian values in BranchingTQP");
    retval = tminlp_->eval_g(n_, x_sol_, new_x, m_, g_vals_);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate constraint values in BranchingTQP");
    retval = tminlp_->eval_jac_g(n_, x_sol_, new_x, m_, nnz_jac_g_,
				 g_jac_irow_, g_jac_jcol_, NULL);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate constraint Jacobian structure in BranchingTQP");
    if (index_style_ == TNLP::FORTRAN_STYLE) {
      for (Index i=0; i<nnz_jac_g_; i++) {
	g_jac_irow_[i]--;
	g_jac_jcol_[i]--;
      }
    }
    retval = tminlp_->eval_jac_g(n_, x_sol_, new_x, m_, nnz_jac_g_,
				 NULL, NULL, g_jac_);
    ASSERT_EXCEPTION(retval, TMINLP_INVALID,
		     "Can't evaluate constraint Jacobian values in BranchingTQP");

    // Get room for "displacement"
    d_ = new Number[n_];
  }

  BranchingTQP::~BranchingTQP()
  {
    delete [] obj_grad_;
    delete [] obj_hess_;
    delete [] obj_hess_irow_;
    delete [] obj_hess_jcol_;
    delete [] g_vals_;
    delete [] g_jac_;
    delete [] g_jac_irow_;
    delete [] g_jac_jcol_;
    delete [] d_;
  }

  bool BranchingTQP::get_starting_point(Index n, bool init_x, Number* x,
      bool init_z, Number* z_L, Number* z_U,
      Index m, bool init_lambda,
      Number* lambda)
  {
    DBG_ASSERT(n==n_);
    if (init_x == true) {
      if(x_init_==NULL)
        return false;
      IpBlasDcopy(n, x_sol_, 1, x, 1);
    }
    if (init_z == true) {
      if(duals_sol_ == NULL)
        return false;
      IpBlasDcopy(n, &duals_sol_[m], 1, z_L, 1);
      IpBlasDcopy(n, &duals_sol_[m + n], 1, z_U, 1);

    }
    if(init_lambda == true) {
      if(duals_sol_ == NULL)
        return false;
      IpBlasDcopy(m_, duals_sol_, 1, lambda, 1);
      for(int i = m_ ; i < m; i++)
      {
        lambda[i] = 0.;
      }
    }

    need_new_warm_starter_ = true;
    return true;
  }

  bool BranchingTQP::eval_f(Index n, const Number* x, bool new_x,
      Number& obj_value)
  {
    DBG_ASSERT(n == n_);
    if (new_x) {
      update_displacement(x);
    }

    obj_value = obj_value_ + IpBlasDdot(n, d_, 1, obj_grad_, 1);
    for (int i=0; i<nnz_h_lag_; i++) {
      Index& irow = obj_hess_irow_[i];
      Index& jcol = obj_hess_jcol_[i];
      if (irow!=jcol) {
	obj_value += obj_hess_[i]*d_[irow]*d_[jcol];
      }
      else {
	obj_value += 0.5*obj_hess_[i]*d_[irow]*d_[irow];
      }
    }

    return true;
  }

  bool BranchingTQP::eval_grad_f(Index n, const Number* x, bool new_x,
				 Number* grad_f)
  {
    DBG_ASSERT(n == n_);
    if (new_x) {
      update_displacement(x);
    }

    IpBlasDcopy(n_, obj_grad_, 1, grad_f, 1);
    for (int i=0; i<nnz_h_lag_; i++) {
      Index& irow = obj_hess_irow_[i];
      Index& jcol = obj_hess_jcol_[i];
      grad_f[irow] += obj_hess_[i]*d_[jcol];
      if (irow!=jcol) {
	grad_f[jcol] += obj_hess_[i]*d_[irow];
      }
    }

    return true;
  }

  bool BranchingTQP::eval_g(Index n, const Number* x, bool new_x,
			    Index m, Number* g)
  {
    DBG_ASSERT(n == n_);
    if (new_x) {
      update_displacement(x);
    }

    IpBlasDcopy(m_, g_vals_, 1, g, 1);
    for (Index i=0; i<nnz_jac_g_; i++) {
      Index& irow = g_jac_irow_[i];
      Index& jcol = g_jac_jcol_[i];
      g[irow] += g_jac_[i]*d_[jcol];
    }

    eval_g_add_linear_cuts(g, x);
    return true;
  }

  bool BranchingTQP::eval_jac_g(Index n, const Number* x, bool new_x,
				Index m, Index nele_jac, Index* iRow,
				Index *jCol, Number* values)
  {
    if (new_x) {
      update_displacement(x);
    }

    if (iRow != NULL) {
      DBG_ASSERT(jCol != NULL);
      DBG_ASSERT(values == NULL);
      if (index_style_ == TNLP::FORTRAN_STYLE) {
	for (Index i=0; i<nnz_jac_g_; i++) {
	  iRow[i] = g_jac_irow_[i] + 1;
	  jCol[i] = g_jac_jcol_[i] + 1;
	}
      }
      else {
	for (Index i=0; i<nnz_jac_g_; i++) {
	  iRow[i] = g_jac_irow_[i];
	  jCol[i] = g_jac_jcol_[i];
	}
      }
    }
    else {
      IpBlasDcopy(nnz_jac_g_, g_jac_, 1, values, 1);
    }

    eval_jac_g_add_linear_cuts(nele_jac, iRow, jCol, values);

    return true;
  }

  bool BranchingTQP::eval_h(Index n, const Number* x, bool new_x,
      Number obj_factor, Index m, const Number* lambda,
      bool new_lambda, Index nele_hess,
      Index* iRow, Index* jCol, Number* values)
  {
    if (new_x) {
      update_displacement(x);
    }
    DBG_ASSERT(nele_hess == nnz_h_lag_);

    if (iRow != NULL) {
      DBG_ASSERT(jCol != NULL);
      DBG_ASSERT(values == NULL);
      if (index_style_ == TNLP::FORTRAN_STYLE) {
	for (Index i=0; i<nele_hess; i++) {
	  iRow[i] = obj_hess_irow_[i] + 1;
	  jCol[i] = obj_hess_jcol_[i] + 1;
	}
      }
      else {
	for (Index i=0; i<nele_hess; i++) {
	  iRow[i] = obj_hess_irow_[i];
	  jCol[i] = obj_hess_jcol_[i];
	}
      }
    }
    else {
      if (obj_factor==0.) {
	const Number zero = 0.;
	IpBlasDcopy(nele_hess, &zero, 0, values, 1);
      }
      else {
	IpBlasDcopy(nele_hess, obj_hess_, 1, values, 1);
	if (obj_factor != 1.) {
	  IpBlasDscal(nele_hess, obj_factor, values, 1);
	}
      }
    }

    return true;
  }

  void BranchingTQP::update_displacement(const Number* x)
  {
    for (Index i=0; i<n_; i++) {
      d_[i] = x[i] - x_sol_[i];
    }
  }

}
// namespace Ipopt

