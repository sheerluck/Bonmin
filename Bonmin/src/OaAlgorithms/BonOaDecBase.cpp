  // (C) Copyright International Business Machines (IBM) 2006
// All Rights Reserved.
// This code is published under the Common Public License.
//
// Authors :
// P. Bonami, International Business Machines
//
// Date :  12/07/2006

#include "BonOaDecBase.hpp"


#include "BonminConfig.h"

#include "OsiClpSolverInterface.hpp"

#include "CbcModel.hpp"
#include "CbcStrategy.hpp"
#ifdef COIN_HAS_CPX
#include "OsiCpxSolverInterface.hpp"
#endif
#include "OsiAuxInfo.hpp"

//The following two are to interupt the solution of sub-mip through CTRL-C
extern CbcModel * OAModel;

namespace Bonmin {

   OaDecompositionBase::OaDecompositionBase
   (OsiTMINLPInterface * nlp,
   OsiSolverInterface * si,
   CbcStrategy * strategy,
   double cbcCutoffIncrement,
   double cbcIntegerTolerance,
   bool leaveSiUnchanged
   )
      :
      CglCutGenerator(),
      nlp_(nlp),
      nSolve_(0),
      lp_(si),
      nLocalSearch_(0),
      handler_(NULL),
      leaveSiUnchanged_(leaveSiUnchanged),
      timeBegin_(0),
      parameters_()
  {
    handler_ = new CoinMessageHandler();
    handler_ -> setLogLevel(2);
    messages_ = OaMessages();
    if (strategy)
      parameters_.setStrategy(*strategy);
    timeBegin_ = CoinCpuTime();
    parameters_.cbcCutoffIncrement_  = cbcCutoffIncrement;
    parameters_.cbcIntegerTolerance_ = cbcIntegerTolerance;
  }

  OaDecompositionBase::OaDecompositionBase
  (const OaDecompositionBase & other)
      :
      CglCutGenerator(other),
      nlp_(other.nlp_),
      lp_(other.lp_),
      nLocalSearch_(0),
      handler_(NULL), 
      messages_(other.messages_),
      leaveSiUnchanged_(other.leaveSiUnchanged_),
      timeBegin_(0),
      parameters_(other.parameters_)
    {
      handler_ = new CoinMessageHandler();
      handler_->setLogLevel(other.handler_->logLevel());
      timeBegin_ = CoinCpuTime();
    }
/// Constructor with default values for parameters
OaDecompositionBase::Parameters::Parameters():
  global_(true),
  addOnlyViolated_(false),
  cbcCutoffIncrement_(1e-06),
  cbcIntegerTolerance_(1e-05),
  localSearchNodeLimit_(0),
  maxLocalSearchPerNode_(0),
  maxLocalSearch_(0),
  maxLocalSearchTime_(3600),
  subMilpLogLevel_(0),
  logFrequency_(1000.),
  strategy_(NULL)
  {}

/** Destructor.*/ 
OaDecompositionBase::~OaDecompositionBase(){
  delete handler_;}

  /// Assign an OsiTMINLPInterface (interface to non-linear problem).
  void
  OaDecompositionBase::assignNlpInterface(OsiTMINLPInterface * nlp)
  {
    nlp_ = nlp;
  }

  /// Assign an OsiSolverInterface (interface to LP solver).
  void
  OaDecompositionBase::assignLpInterface(OsiSolverInterface * si)
  {
    lp_ = si;
  }



/// Constructor with default values for parameters
OaDecompositionBase::Parameters::Parameters(const Parameters & other):
  global_(other.global_),
  addOnlyViolated_(other.addOnlyViolated_),
  cbcCutoffIncrement_(other.cbcCutoffIncrement_),
  cbcIntegerTolerance_(other.cbcIntegerTolerance_),
  localSearchNodeLimit_(other.localSearchNodeLimit_),
  maxLocalSearchPerNode_(other.maxLocalSearchPerNode_),
  maxLocalSearch_(other.maxLocalSearch_),
  maxLocalSearchTime_(other.maxLocalSearchTime_),
  subMilpLogLevel_(other.subMilpLogLevel_),
  logFrequency_(other.logFrequency_),
  strategy_(NULL)
  {
    if(other.strategy_)
      strategy_ = other.strategy_->clone();
  }
 

/** Constructor */
OaDecompositionBase::SubMipSolver::SubMipSolver(OsiSolverInterface * lp,
             const CbcStrategy * strategy):
 lp_(lp),
 clp_(NULL),
 cpx_(NULL),
 cbc_(NULL),
 lowBound_(-DBL_MAX),
 optimal_(false),
 integerSolution_(NULL),
 strategy_(NULL)
 {
   clp_ = (lp_ == NULL)? NULL :
         dynamic_cast<OsiClpSolverInterface *>(lp_);
#ifdef COIN_HAS_CPX
   cpx_ = (lp_ == NULL)? NULL :
         dynamic_cast<OsiCpxSolverInterface *>(lp_);
#endif
   if(strategy) strategy_ = strategy->clone();
 }
 OaDecompositionBase::SubMipSolver::~SubMipSolver(){
   if(strategy_) delete strategy_;
   if(integerSolution_) delete [] integerSolution_;
   if(cbc_) delete cbc_;
  }

/** Assign lp solver. */
void 
OaDecompositionBase::SubMipSolver::setLpSolver(OsiSolverInterface * lp)
{
  lp_ = lp;
  clp_ = (lp_ == NULL) ? NULL :
         dynamic_cast<OsiClpSolverInterface *>(lp_);
#ifdef COIN_HAS_CPX
  cpx_ = (lp_ == NULL) ? NULL :
         dynamic_cast<OsiCpxSolverInterface *>(lp_);
#endif
  lowBound_ = -DBL_MAX;
  optimal_ = false;
  if(integerSolution_){
    delete [] integerSolution_;
    integerSolution_ = NULL;
  }
}



void
OaDecompositionBase::SubMipSolver::performLocalSearch(double cutoff, int loglevel, double maxTime,
                                                  int maxNodes)
{
  if(clp_)
  {
    if(!strategy_)
     strategy_ = new CbcStrategyDefault(1,0,0, loglevel);

    OsiBabSolver empty;
    if(cbc_) delete cbc_;
    OAModel = cbc_ = new CbcModel(*clp_); 
    cbc_->solver()->setAuxiliaryInfo(&empty);

    //Change Cbc messages prefixes
    strcpy(cbc_->messagesPointer()->source_,"OaCbc");

    clp_->resolve();
    cbc_->setLogLevel(loglevel);
    cbc_->solver()->messageHandler()->setLogLevel(0);
    cbc_->setStrategy(*strategy_);
    cbc_->setMaximumNodes(maxNodes);
    cbc_->setMaximumSeconds(maxTime);
    cbc_->setCutoff(cutoff);

    cbc_->branchAndBound();
    OAModel = NULL;
    lowBound_ = cbc_->getBestPossibleObjValue();

    if(cbc_->isProvenOptimal() || cbc_->isProvenInfeasible())
      optimal_ = true; 
    else optimal_ = false;

    if(cbc_->getSolutionCount()){
      if(!integerSolution_)
        integerSolution_ = new double[lp_->getNumCols()];
      CoinCopyN(cbc_->bestSolution(), lp_->getNumCols(), integerSolution_);
    }
    else if(integerSolution_) {
      delete [] integerSolution_;
      integerSolution_ = NULL;
    }
    nodeCount_ = cbc_->getNodeCount();
    iterationCount_ = cbc_->getIterationCount();
  }
  else {
    lp_->messageHandler()->setLogLevel(loglevel);
#ifdef COIN_HAS_CPX
    if(cpx_){
      CPXENVptr env = cpx->getEnvironmentPtr();
      CPXsetintparam(env, CPX_PARAM_NODELIM, maxNodes);
      CPXsetdblparam(env, CPX_PARAM_TILIM, maxTime);
      CPXsetdblparam(env, CPX_PARAM_CUTUP, cutoff);
      //CpxModel = cpx_;
    }
    else
#endif
    {
     throw CoinError("Unsuported solver, for local searches you should use clp or cplex",
                     "performLocalSearch",
                     "OaDecompositionBase::SubMipSolver");
    } 

    lp_->branchAndBound();

#ifdef COIN_HAS_CPX
    if(cpx_)
    {
      //CpxModel = NULL;
      CPXENVptr env = cpx_->getEnvironmentPtr();
      CPXLPptr cpxlp = cpx_->cpx->getLpPtr(OsiCpxSolverInterface::KEEPCACHED_ALL);
      
      int status = CPXgetbestobjval(env, cpxlp, lowBound_);
      status |= CPXgetnodecnt(env , cpxlp, nodeCount_); 
      status |= CPXgetmipitcnt(env , cpxlp, iterationCount_); 
      if (status)
        throw CoinError("Error in getting some CPLEX information","OaDecompositionBase::SubMipSolver","performLocalSearch");
    }
#endif

    if(lp_->getFractionalIndices().size() == 0){
      if(!integerSolution_)
        integerSolution_ = new double[lp_->getNumCols()];
      CoinCopyN(lp_->getColSolution(), lp_->getNumCols() , integerSolution_);      
    }
    else if(integerSolution_){
      delete [] integerSolution_;
      integerSolution_ = NULL;
    } 
  }
}

OaDecompositionBase::solverManip::solverManip
         (OsiSolverInterface * si,
          bool saveNumRows,
          bool saveBasis,
          bool saveBounds,
          bool saveCutoff):
  si_(si),
  initialNumberRows_(-1),
  colLower_(NULL),
  colUpper_(NULL),
  warm_(NULL),
  cutoff_(DBL_MAX),
  deleteSolver_(false)
{
  getCached();
  if(saveNumRows)
    initialNumberRows_ = numrows_;
  if(saveBasis)
    warm_ = si->getWarmStart();
  if(saveBounds){
    colLower_ = new double[numcols_];
    colUpper_ = new double[numcols_];
    CoinCopyN(si->getColLower(), numcols_ , colLower_);
    CoinCopyN(si->getColUpper(), numcols_ , colUpper_);
  }
  if(saveCutoff)
   si->getDblParam(OsiDualObjectiveLimit, cutoff_);
}


OaDecompositionBase::solverManip::solverManip
         (const OsiSolverInterface & si):
  si_(NULL),
  initialNumberRows_(-1),
  colLower_(NULL),
  colUpper_(NULL),
  warm_(NULL),
  cutoff_(DBL_MAX),
  deleteSolver_(true)
{
  si_ = si.clone();
  getCached();
}

OaDecompositionBase::solverManip::~solverManip(){
  if(warm_) delete warm_;
  if(colLower_) delete [] colLower_;
  if(colUpper_) delete [] colUpper_;
  if(deleteSolver_) delete si_;
}

void
OaDecompositionBase::solverManip::restore(){
  if(initialNumberRows_ >= 0){
    int nRowsToDelete = numrows_ - initialNumberRows_;
    int * rowsToDelete = new int[nRowsToDelete];
    for (int i = 0 ; i < nRowsToDelete ; i++) {
      rowsToDelete[i] = i + initialNumberRows_;
    }
    si_->deleteRows(nRowsToDelete, rowsToDelete);
    delete [] rowsToDelete;
    numrows_ -= nRowsToDelete;
  }

  if(colLower_){
    si_->setColLower(colLower_);
  }
  
  if(colUpper_){
    si_->setColUpper(colUpper_);
  }

  if(cutoff_<DBL_MAX){
     si_->setDblParam(OsiDualObjectiveLimit, cutoff_);
  }

  if(warm_){
     if(si_->setWarmStart(warm_)==false){
       throw CoinError("Fail restoring the warm start at the end of procedure",
                "restore","OaDecompositionBase::SaveSolverState") ;
     }
  }
  getCached();
}


/// Check for integer feasibility of a solution return 1 if it is
bool 
OaDecompositionBase::integerFeasible(const double * sol, int numcols) const{
  for(int i = 0 ; i < numcols ; i++)
  {
    if(nlp_->isInteger(i)) {
      if(fabs(sol[i]) - floor(sol[i] + 0.5) >
         parameters_.cbcIntegerTolerance_) {
         return false;
      }
    }
  }
  return true;
}

/** Fix integer variables in si to their values in colsol
\todo Handle SOS type 2.*/
void 
OaDecompositionBase::solverManip::fixIntegers(const double * colsol) {
  for (int i = 0; i < numcols_; i++) {
    if (si_->isInteger(i)) {
      double  value =  colsol[i];
      if(value - floor(value+0.5) > 1e-04){
	std::cerr<<"Error not integer valued solution"<<std::endl;
      }
      value = floor(value+0.5);
      value = max(colLower_[i],value);
      value = min(value, colUpper_[i]);

      if (fabs(value) > 1e10) {
        std::cerr<<"ERROR: Can not fix variable in nlp because it has too big a value ("<<value
        <<") at optimium of LP relaxation. You should try running the problem with B-BB"<<std::endl;
        throw -1;
      }
#ifdef OA_DEBUG
      //         printf("xx %d at %g (bounds %g, %g)",i,value,nlp_->getColLower()[i],
      //                nlp_->getColUpper()[i]);
      std::cout<<(int)value;
#endif
      si_->setColLower(i,value);
      si_->setColUpper(i,value);
    }
  }
#ifdef OA_DEBUG
  std::cout<<std::endl;
#endif
}

/** Check if solution in solver is the same as colsol on integer variables. */
bool 
OaDecompositionBase::solverManip::isDifferentOnIntegers(const double * colsol){
  const double * siSol= si_->getColSolution();
  for (int i = 0; i < numcols_ ; i++) {
     if (si_->isInteger(i) && fabs(siSol[i] - colsol[i])>0.001)
       return true;
  }
  return false;
}

/** Clone the state of another solver (bounds, cutoff, basis).*/
void
OaDecompositionBase::solverManip::cloneOther(const OsiSolverInterface &si){
  //Install current active cuts into local solver
  int numberCutsToAdd = si.getNumRows();
  numberCutsToAdd -= numrows_;
  if (numberCutsToAdd > 0)//Have to install some cuts
  {
    CoinPackedVector * * addCuts = new CoinPackedVector *[numberCutsToAdd];
    for (int i = 0 ; i < numberCutsToAdd ; i++)
    {
      addCuts[i] = new CoinPackedVector;
    }
    //Get the current matrix and fill the addCuts
    const CoinPackedMatrix * mat = si.getMatrixByCol();
    const CoinBigIndex * start = mat->getVectorStarts();
    const int * length = mat->getVectorLengths();
    const double * elements = mat->getElements();
    const int * indices = mat->getIndices();
    for (int i = 0 ; i <= numcols_ ; i++)
      for (int k = start[i] ; k < start[i] + length[i] ; k++)
      {
        if (indices[k] >= numrows_) {
          addCuts[ indices[k] - numrows_ ]->insert(i, elements[k]);
        }
      }
    si_->addRows(numberCutsToAdd, (const CoinPackedVectorBase * const *) addCuts, si.getRowLower() + numrows_,
        si.getRowUpper() + numrows_);
  }
  else if (numberCutsToAdd < 0)//Oups some error
  {
    std::cerr<<"Internal error in OACutGenerator2 : number of cuts wrong"<<std::endl;
  }

  si_->setColLower(si.getColLower());
  si_->setColUpper(si.getColUpper());
  //Install basis in problem
  CoinWarmStart * warm = si.getWarmStart();
  if (si_->setWarmStart(warm)==false)
  {
    delete warm;
    throw CoinError("Fail installing the warm start in the subproblem",
        "generateCuts","OACutGenerator2") ;
  }
  delete warm;
  //put the cutoff
  double cutoff;
  si.getDblParam(OsiDualObjectiveLimit, cutoff);
  si_->setDblParam(OsiDualObjectiveLimit, cutoff);
  si_->resolve();

  numrows_ = si.getNumRows();
#ifdef OA_DEBUG

  std::cout<<"Resolve with hotstart :"<<std::endl
  <<"number of iterations(should be 0) : "<<lp_->getIterationCount()<<std::endl
  <<"Objective value and diff to original : "<<lp_->getObjValue()<<", "
  <<fabs(si_->getObjValue() - si.getObjValue())<<std::endl;
  for (int i = 0 ; i <= numcols ; i++)
  {
    if (fabs(si.getColSolution()[i]-si_->getColSolution()[i])>1e-08) {
      std::cout<<"Diff between solution at node and solution with local solver : "<<fabs(si.getColSolution()[i]-lp_->getColSolution()[i])<<std::endl;
    }
  }
#endif
 
}


/** Install cuts in solver. */
void 
OaDecompositionBase::solverManip::installCuts(const OsiCuts& cs, int numberCuts){
  int numberCutsBefore = cs.sizeRowCuts() - numberCuts;

  CoinWarmStartBasis * basis
  = dynamic_cast<CoinWarmStartBasis*>(si_->getWarmStart()) ;
  assert(basis != NULL); // make sure not volume
  basis->resize(numrows_ + numberCuts,numcols_ + 1) ;
  for (int i = 0 ; i < numberCuts ; i++) {
    basis->setArtifStatus(numrows_ + i,
        CoinWarmStartBasis::basic) ;
  }

  const OsiRowCut ** addCuts = new const OsiRowCut * [numberCuts] ;
  for (int i = 0 ; i < numberCuts ; i++) {
    addCuts[i] = &cs.rowCut(i + numberCutsBefore) ;
  }
  si_->applyRowCuts(numberCuts,addCuts) ;
  numrows_ += numberCuts;
  delete [] addCuts ;
  if (si_->setWarmStart(basis) == false) {
    delete basis;
    throw CoinError("Fail setWarmStart() after cut installation.",
        "generateCuts","OACutGenerator2") ;
  }
  delete basis;
}

/** Standard cut generation methods. */
void 
OaDecompositionBase::generateCuts(const OsiSolverInterface &si,  OsiCuts & cs,
                          const CglTreeInfo info) const{
    if (nlp_ == NULL) {
      std::cerr<<"Error in cut generator for outer approximation no NLP ipopt assigned"<<std::endl;
      throw -1;
    }

    // babInfo is used to communicate with the b-and-b solver (Cbc or Bcp).
    OsiBabSolver * babInfo = dynamic_cast<OsiBabSolver *> (si.getAuxiliaryInfo());

    const int numcols = nlp_->getNumCols();

    //Get the continuous solution
    const double *colsol = si.getColSolution();


    //Check integer infeasibility
    bool isInteger = integerFeasible(colsol, numcols);

    SubMipSolver * subMip = NULL;

    if (!isInteger) {
      if (doLocalSearch())//create sub mip solver.
      {
        subMip = new SubMipSolver(lp_, parameters_.strategy());
      }
      else {
        return;
      }
    }


    //If we are going to modify things copy current information to restore it in the end


    //get the current cutoff
    double cutoff;
    si.getDblParam(OsiDualObjectiveLimit, cutoff);

    // Save solvers state if needed
    solverManip nlpManip(nlp_, false, false, true, false);

    solverManip * lpManip = NULL; 
    if(lp_ != NULL){
      if(lp_!=&si){
        lpManip = new solverManip(lp_, true, false, false, true);
        lpManip->cloneOther(si);
      }
      else{
#if 0
        throw CoinError("Not allowed to modify si in a cutGenerator",
          "OACutGenerator2","generateCuts");
#else
         lpManip = new solverManip(lp_, true, leaveSiUnchanged_, true, true);
#endif
      }
    }
    else{
      lpManip = new solverManip(si);
    }
    double milpBound = performOa(cs, nlpManip, *lpManip, subMip, babInfo, cutoff);

    //Transmit the bound found by the milp
    {
      if (milpBound>-1e100)
      {
        // Also store into solver
        if (babInfo)
          babInfo->setMipBound(milpBound);
      }
    }  //Clean everything :

    //free subMip
    if (subMip!= NULL) {
      delete subMip;
      subMip = NULL;
    }

    //  Reset the two solvers
    if(leaveSiUnchanged_)
      lpManip->restore();
    delete lpManip;
    nlpManip.restore();
    return;
}

void
OaDecompositionBase::solverManip::getCached(){
  numrows_ = si_->getNumRows();
  numcols_ = si_->getNumCols();
  siColLower_ = si_->getColLower();
  siColUpper_ = si_->getColUpper();
}


/** Solve the nlp and do output return true if feasible*/
bool 
OaDecompositionBase::solveNlp(OsiBabSolver * babInfo, double cutoff) const{
      nSolve_++;
      nlp_->resolve();
      bool return_value = false;
      if (nlp_->isProvenOptimal()) {
        handler_->message(FEASIBLE_NLP, messages_)
        <<nlp_->getIterationCount()
        <<nlp_->getObjValue()<<CoinMessageEol;

#ifdef OA_DEBUG
        const double * colsol2 = nlp_->getColSolution();
        debug_.checkInteger(colsol2,numcols,std::cerr);
#endif

        if ((nlp_->getObjValue() < cutoff) ) {
          handler_->message(UPDATE_UB, messages_)
          <<nlp_->getObjValue()
          <<CoinCpuTime()-timeBegin_
          <<CoinMessageEol;

          return_value = true;
          // Also pass it to solver
          if (babInfo) {
	    int numcols = nlp_->getNumCols();
            double * lpSolution = new double[numcols + 1];
            CoinCopyN(nlp_->getColSolution(), numcols, lpSolution);
            lpSolution[numcols] = nlp_->getObjValue();
            babInfo->setSolution(lpSolution,
                numcols + 1, lpSolution[numcols]);
            delete [] lpSolution;
          }
          else {
            printf("No auxiliary info in nlp solve!\n");
            throw -1;
          }
        }
      }
      else if (nlp_->isAbandoned() || nlp_->isIterationLimitReached()) {
        std::cerr<<"Unsolved NLP... exit"<<std::endl;
      }
      else {
        handler_->message(INFEASIBLE_NLP, messages_)
        <<nlp_->getIterationCount()
        <<CoinMessageEol;
      }
      return return_value;
}



#ifdef OA_DEBUG
bool 
OaDecompositionBase::OaDebug::checkInteger(const double * colsol, int numcols, ostream & os) const{
  for(int i = 0 ; i < numcols ; i++)
  {
    if(nlp_->isInteger(i)) {
      if(fabs(sol[i]) - floor(sol[i] + 0.5) >
         parameters_.cbcIntegerTolerance_) {
           std::cerr<<"Integer infeasible point (should not be), integer infeasibility for variable "<<i
                    <<" is, "<<fabs(colsol2[i] - floor(colsol2[i] + 0.5))<<std::endl;
      }
    }
    return true;
  }

}

void 
OaDecompositionBase::OaDebug::printEndOfProcedureDebugMessage(const OsiCuts &cs, 
                                     bool foundSolution, 
                                     double milpBound, 
                                     bool isInteger, 
                                     bool feasible, 
                                     std::ostream & os){
    std::cout<<"------------------------------------------------------------------"
             <<std::endl;
    std::cout<<"OA procedure finished"<<std::endl;
    std::cout<<"Generated "<<cs.sizeRowCuts()<<std::endl;
    if (foundSolution)
      std::cout <<"Found NLP-integer feasible solution of  value : "<<cutoff<<std::endl;
    std::cout<<"Current MILP lower bound is : "<<milpBound<<std::endl;
    std::cout<<"-------------------------------------------------------------------"<<std::endl;
    std::cout<<"Stopped because : isInteger "<<isInteger<<", feasible "<<feasible<<std::endl<<std::endl;

}



#endif
}/* End namespace Bonmin. */

