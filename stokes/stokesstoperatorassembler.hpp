#ifndef STOKESSTOPERATORASSEMBLER_HPP
#define STOKESSTOPERATORASSEMBLER_HPP

#include <mpi.h>
#include "mfem.hpp"
#include <string>



using namespace mfem;




// "Inverse" of space-time matrix
class SpaceTimeSolver: public Operator{

private:
	const MPI_Comm _comm;
	int _numProcs;
	int _myRank;
  
  // relevant operators
  const PetscParMatrix *_F;
  const SparseMatrix   *_M;

	// solvers for relevant operator (with corresponding preconditioner, if iterative)
  PetscLinearSolver *_Fsolve;

  // dofs for velocity (useful not to dirty dirichlet BC in the solution procedure)
 	const Array<int> _essVhTDOF;

  mutable HypreParVector* _X;
  mutable HypreParVector* _Y;

	const bool _verbose;


public:

	SpaceTimeSolver( const MPI_Comm& comm, const SparseMatrix* F=NULL, const SparseMatrix* M=NULL,
		               const Array<int>& essVhTDOF=Array<int>(), bool verbose=false);

  void Mult( const Vector& x, Vector& y ) const;

  void SetF( const SparseMatrix* F );
  void SetM( const SparseMatrix* M );

	~SpaceTimeSolver();

private:
	void SetFSolve();


}; //SpaceTimeSolver







// Approximation to pressure Schur complement
class StokesSTPreconditioner: public Operator{

private:
	const MPI_Comm _comm;
	int _numProcs;
	int _myRank;

  double _dt;
  double _mu;

  // relevant operators
  const PetscParMatrix *_Ap;
  const PetscParMatrix *_Mp;

  // solvers for relevant operators
  PetscLinearSolver *_Asolve;
  PetscLinearSolver *_Msolve;

  // dofs for pressure (useful not to dirty dirichlet BC in the solution procedure)
	const Array<int> _essQhTDOF;

	const bool _verbose;


public:

	StokesSTPreconditioner( const MPI_Comm& comm, double dt, double mu,
		                      const SparseMatrix* Ap = NULL, const SparseMatrix* Mp = NULL,
		                      const Array<int>& essQhTDOF = Array<int>(),
		                      bool verbose = false );
	~StokesSTPreconditioner();



  void Mult( const Vector& x, Vector& y ) const;

  void SetAp( const SparseMatrix* Ap );
  void SetMp( const SparseMatrix* Mp );

private:
	void SetMpSolve();
	void SetApSolve();



}; //StokesSTPreconditioner














/** Placeholder class for handling space-time stokes
- at least until I sort out how to make Ben's class work*/
class StokesSTOperatorAssembler{

private:
	
	// info for parallelisation
	const MPI_Comm _comm;
	int _numProcs;
	int _myRank;


	// problem parameters
	const double _dt; 	//time step (constant)
	const double _mu;		//viscosity
  int _dim;						//domain dimension (R^d)
	void(  *_fFunc)( const Vector &, double, Vector & );	// function returning forcing term for velocity (time-dep)
	double(*_gFunc)( const Vector &, double )          ;  // function returning forcing term for pressure (time-dep)
	void(  *_nFunc)( const Vector &, double, Vector & );  // function returning mu * du/dn (time-dep, used to implement BC)
	void(  *_uFunc)( const Vector &, double, Vector & );	// function returning velocity solution (time-dep, used to implement IC, and compute error)
	double(*_pFunc)( const Vector &, double )          ;  // function returning pressure solution (time-dep, used to implement IC and BC, and compute error)


	// info on FE
  Mesh *_mesh;
  FiniteElementCollection *_VhFEColl;
  FiniteElementCollection *_QhFEColl;
  FiniteElementSpace      *_VhFESpace;
  FiniteElementSpace      *_QhFESpace;
  const int _ordU;
  const int _ordP;

	Array<int> _essVhTDOF;
	Array<int> _essQhTDOF;

  // relevant operators and corresponding matrices
  // - blocks for single time-steps
  BilinearForm      *_fuVarf;
  MixedBilinearForm *_bVarf;
  SparseMatrix _Mu;
  SparseMatrix _Fu;
  SparseMatrix _Mp;
  SparseMatrix _Ap;
  SparseMatrix _B;
  bool _MuAssembled;
  bool _FuAssembled;
  bool _MpAssembled;
  bool _ApAssembled;
  bool _BAssembled;

  // - space-time blocks
  HYPRE_IJMatrix _FF;								// Space-time velocity block
  HYPRE_IJMatrix _BB;								// space-time -div block
  StokesSTPreconditioner *_pSchur;  // Approximation to space-time pressure Schur complement
  SpaceTimeSolver        *_FFinv;   // Space-time velocity block solver
  bool _FFAssembled;
  bool _BBAssembled;
	bool _pSAssembled;
	bool _FFinvAssembled;


  // // - full-fledged operators
  // BlockOperator _STstokes;					 // space-time stokes operator
  // BlockOperator _STstokesPrec;      // space-time block preconditioner


	const bool _verbose;



public:
	StokesSTOperatorAssembler( const MPI_Comm& comm, const std::string &meshName, const int refLvl,
		                         const int ordU, const int ordP, const double dt, const double mu,
		                         void(  *f)(const Vector &, double, Vector &),
		                         double(*g)(const Vector &, double ),
		                         void(  *n)(const Vector &, double, Vector &),
		                         void(  *u)(const Vector &, double, Vector &),
		                         double(*p)(const Vector &, double ),
		                         bool verbose );
	~StokesSTOperatorAssembler();


	// void AssembleOperator( HypreParMatrix*& FFF, HypreParMatrix*& BBB );

	void AssembleSystem( HypreParMatrix*& FFF,  HypreParMatrix*& BBB,
		                   HypreParVector*& frhs, HypreParVector*& grhs,
		                   HypreParVector*& IGu,  HypreParVector*& IGp  );
	void AssemblePreconditioner( Operator*& Finv, Operator*& XXX );

	// void AssembleRhs( HypreParVector*& frhs, HypreParVector*& grhs );

	void ApplySTOperatorVelocity( const HypreParVector*& u, HypreParVector*& res );


	void ExactSolution( HypreParVector*& u, HypreParVector*& p );

	void TimeStepVelocity( const HypreParVector& rhs, HypreParVector*& sol );
	void TimeStepPressure( const HypreParVector& rhs, HypreParVector*& sol );

	void GetMeshSize( double& h_min, double& h_max, double& k_min, double& k_max ) const;
	void ComputeL2Error( const HypreParVector& uh, const HypreParVector& ph );
	void SaveSolution(   const HypreParVector& uh, const HypreParVector& ph );
	void SaveExactSolution( );
	void PrintMatrices( const std::string& filename ) const;

private:
	// assemble blocks for single time-step 
	void AssembleFu();
	void AssembleMu();
	void AssembleAp();
	void AssembleMp();
	void AssembleBvarf();

	// assemble blocks for whole Space-time operators 
	void AssembleFF();
	void AssembleBB();
	void AssemblePS();
	void AssembleFFinv();


	void TimeStep( const SparseMatrix &F, const SparseMatrix &M, const HypreParVector& rhs, HypreParVector*& sol );







}; //StokesSTOperatorAssembler












#endif //STOKESSTOPERATORASSEMBLER