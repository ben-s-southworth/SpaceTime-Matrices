#ifndef INCOMPRESSIBLEMHD2DINTEGRATOR_HPP
#define INCOMPRESSIBLEMHD2DINTEGRATOR_HPP

#include "mfem.hpp"
namespace mfem{

/** 2D incompressible MHD equations in vector potential formulation,
   augmented with the extra variable z = (∇·∇A),
   ⎧ dt(u) + (u·∇)u - mu ∇·∇u + ∇p + 1/mu0 z ∇A = f
   ⎨                                        ∇·u = g
   |                                 z - (∇·∇A) = 0
   ⎩                    dt(A) + u·∇A - eta ∇·∇A = h

  Notice this integrator should be added BOTH as a domain integrator
  (to compute the main bilinear forms) AND as a boundary face integrator
  (to compute the Neumann boundary term stemming from the third equation)

	NB: This integrator assumes that the FE spaces are polynomial
	     (particularly for the velocity variable)
  NB: All equations in the system above are rescaled by dt, except for the one in z:
       I think that not doing so will make it harder for smaller dt to solve the
       system with a comparable degree of accuracy for all the equations
*/
class IncompressibleMHD2DIntegrator : public BlockNonlinearFormIntegrator{
private:
	const double _dt;
	const double _mu;
	const double _mu0;
	const double _eta;

  // to include stabilisation terms
  const bool   _stab;
  VectorCoefficient *_f;
  Coefficient       *_h;


  // for stabilisation parameters
  const double C1 = 1.;
  const double C2 = 10.;


	Vector shape_u, shape_p, shape_z, shape_a, shape_Dan, nor, ni, nh;
  DenseMatrix shape_Du, shape_Dp, shape_Dz, shape_Da,
  						gshape_u, gshape_p, gshape_z, gshape_a;

  DenseMatrix uCoeff;


public:
  IncompressibleMHD2DIntegrator( double dt, double mu, double mu0, double eta, bool stab=false, VectorCoefficient* f=NULL, Coefficient* h=NULL ):
                                    _dt(dt),   _mu(mu),  _mu0(mu0),  _eta(eta), _stab(stab), _f(f), _h(h){};//std::cout<<"Warning: setting tau to 1 for SUPG\n";};

  // virtual double GetElementEnergy(const Array<const FiniteElement *>&el,
  //                                 ElementTransformation &Tr,
  //                                 const Array<const Vector *> &elfun);

  /// Perform the local action of the NonlinearFormIntegrator
  virtual void AssembleElementVector(const Array<const FiniteElement *> &el,
                                     ElementTransformation &Tr,
                                     const Array<const Vector *> &elfun,
                                     const Array<Vector *> &elvec);

  /// Assemble the local gradient matrix
  virtual void AssembleElementGrad(const Array<const FiniteElement*> &el,
                                   ElementTransformation &Tr,
                                   const Array<const Vector *> &elfun,
                                   const Array2D<DenseMatrix *> &elmats);

  virtual void AssembleFaceGrad ( const Array< const FiniteElement * > &  el1,
                                  const Array< const FiniteElement * > &  el2,
                                  FaceElementTransformations &  Tr,
                                  const Array< const Vector * > &   elfun,
                                  const Array2D< DenseMatrix * > &  elmats ); 

  virtual void AssembleFaceVector(const Array< const FiniteElement * > &  el1,
                                  const Array< const FiniteElement * > &  el2,
                                  FaceElementTransformations &  Trans,
                                  const Array< const Vector * > &   elfun,
                                  const Array< Vector * > &   elvecs );


  /// Get adequate GLL rule for element integration
  const IntegrationRule& GetRule(const Array<const FiniteElement *> &fe, ElementTransformation &T);

  void GetTaus(const Vector& u, const Vector& gA, const DenseMatrix& Jinv, double& tauU, double& tauA) const; // stabilisation coefficients
  
  /// Assembles the contravariant metric tensor of the transformation from local to physical coordinates
  void GetGC( const DenseMatrix& Jinv, DenseMatrix& GC ) const;


};


} //namespace mfem

#endif //INCOMPRESSIBLEMHD2DINTEGRATOR_HPP

