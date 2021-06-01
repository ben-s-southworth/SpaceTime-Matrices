#include "imhd2dtimeintegrator.hpp"
#include "imhd2dspaceintegrator.hpp"

namespace mfem{

void IncompressibleMHD2DTimeIntegrator::AssembleElementGrad(
  const Array<const FiniteElement*> &el,
  ElementTransformation &Tr,
  const Array<const Vector *> &elfun,
  const Array2D<DenseMatrix *> &elmats){

  int dof_u = el[0]->GetDof();
  int dof_p = el[1]->GetDof();
  int dof_z = el[2]->GetDof();
  int dof_a = el[3]->GetDof();

  int dim = el[0]->GetDim();

  elmats(0,0)->SetSize(dof_u*dim, dof_u*dim);  *elmats(0,0) = 0.0;
  elmats(0,1)->SetSize(dof_u*dim, dof_p);      *elmats(0,1) = 0.0;
  elmats(0,2)->SetSize(dof_u*dim, dof_z);      *elmats(0,2) = 0.0;
  elmats(0,3)->SetSize(dof_u*dim, dof_a);      *elmats(0,3) = 0.0;
  elmats(1,0)->SetSize(dof_p,     dof_u*dim);  *elmats(1,0) = 0.0;
  elmats(1,1)->SetSize(dof_p,     dof_p);      *elmats(1,1) = 0.0;
  elmats(1,2)->SetSize(dof_p,     dof_z);      *elmats(1,2) = 0.0;
  elmats(1,3)->SetSize(dof_p,     dof_a);      *elmats(1,3) = 0.0;
  elmats(2,0)->SetSize(dof_z,     dof_u*dim);  *elmats(2,0) = 0.0;
  elmats(2,1)->SetSize(dof_z,     dof_p);      *elmats(2,1) = 0.0;
  elmats(2,2)->SetSize(dof_z,     dof_z);      *elmats(2,2) = 0.0;
  elmats(2,3)->SetSize(dof_z,     dof_a);      *elmats(2,3) = 0.0;
  elmats(3,0)->SetSize(dof_a,     dof_u*dim);  *elmats(3,0) = 0.0;
  elmats(3,1)->SetSize(dof_a,     dof_p);      *elmats(3,1) = 0.0;
  elmats(3,2)->SetSize(dof_a,     dof_z);      *elmats(3,2) = 0.0;
  elmats(3,3)->SetSize(dof_a,     dof_a);      *elmats(3,3) = 0.0;

  shape_u.SetSize(dof_u);
  shape_a.SetSize(dof_a);

  if ( _stab ){  
    shape_Du.SetSize(dof_u, dim);
    shape_Dp.SetSize(dof_p, dim);
    shape_Da.SetSize(dof_a, dim);

    gshape_u.SetSize(dof_u, dim);
    gshape_p.SetSize(dof_p, dim);
    gshape_a.SetSize(dof_a, dim);
  }

  const IntegrationRule& ir = GetRule(el, Tr);

  for (int i = 0; i < ir.GetNPoints(); ++i){

    // compute quantities specific to this integration point
    // - Jacobian-related
    const IntegrationPoint &ip = ir.IntPoint(i);
    Tr.SetIntPoint(&ip);
    const double detJ = Tr.Weight();

    // - basis functions evaluations (on reference element)
    el[0]->CalcShape(  ip, shape_u  );
    el[3]->CalcShape(  ip, shape_a  );

    double scale =0.;


    //***********************************************************************
    // u,u block
    //***********************************************************************
    // Mass (eventually rescaled by 1/dt) -----------------------------------
    DenseMatrix tempuu(dof_u);
    MultVVt(shape_u, tempuu);
    scale = ip.weight * detJ;
// #ifndef MULT_BY_DT
//     scale*= 1./_dt;
// #endif
    tempuu *= scale;

    // this contribution is block-diagonal: for each physical dimension, I can just copy it
    for (int k = 0; k < dim; k++){
      elmats(0,0)->AddMatrix( tempuu, dof_u*k, dof_u*k );
    }


    //***********************************************************************
    // A,A block
    //***********************************************************************
    // Mass (eventually rescaled by 1/dt) -----------------------------------
    scale = ip.weight * detJ;
// #ifndef MULT_BY_DT
//     scale *= 1./_dt;
// #endif
    AddMult_a_VVt( scale, shape_a, *(elmats(3,3)) );






    //***********************************************************************
    //***********************************************************************
    // IF STABILIZATION IS INCLUDED, I NEED EXTRA TERMS!
    //***********************************************************************
    //***********************************************************************
    if ( _stab ){

      DenseMatrix adJ(dim);
      CalcAdjugate(Tr.Jacobian(), adJ);

      el[0]->CalcDShape( ip, shape_Du );
      el[1]->CalcDShape( ip, shape_Dp );
      el[3]->CalcDShape( ip, shape_Da );
      // - basis functions gradient evaluations (on physical element)
      Mult(shape_Du, adJ, gshape_u);  // NB: this way they are all multiplied by detJ already!
      Mult(shape_Dp, adJ, gshape_p);
      Mult(shape_Da, adJ, gshape_a);
      // - function evaluations
      Vector wEval(dim);
      _w->Eval(wEval, Tr, ip);
      Vector gcEval(dim);
      _c->GetGridFunction()->GetGradient( Tr, gcEval );
      // gshape_a.MultTranspose( *(elfun[3]), gAeval ); // ∇A

      double tu=0., ta=0.;
      IncompressibleMHD2DSpaceIntegrator::GetTaus( wEval, gcEval, Tr.InverseJacobian(), _dt, _mu, _mu0, _eta, tu, ta );


      //***********************************************************************
      // u,u block
      //***********************************************************************
      // I need to test the residual of the momentum equation against tu*(w·∇)v
      // dtu term -------------------------------------------------------------
      scale = tu * ip.weight;
  // #ifndef MULT_BY_DT
  //     scale*= 1./_dt;
  // #endif
      DenseMatrix tempuu(dof_u);
      Vector wgu(dof_u);
      gshape_u.Mult(wEval, wgu);
      MultVWt(wgu, shape_u, tempuu);
      tempuu *= scale;
      // -- this contribution is block-diagonal: for each physical dimension, I can just copy it
      for (int k = 0; k < dim; k++){
        elmats(0,0)->AddMatrix( tempuu, dof_u*k, dof_u*k );
      }


      //***********************************************************************
      // p,u block
      //***********************************************************************
      // I need to test the residual of the momentum equation against tu*∇q ***
      // dtu term -------------------------------------------------------------
      // -- this contribution is block-diagonal
      scale = tu * ip.weight;
  // #ifndef MULT_BY_DT
  //     scale*= 1./_dt;
  // #endif
      for (int k = 0; k < dim; k++){
        Vector tempgpi(dof_p);
        DenseMatrix tempgpu(dof_p,dof_u);
        gshape_p.GetColumn(k,tempgpi);
        MultVWt(tempgpi, shape_u, tempgpu);
        elmats(1,0)->AddMatrix( scale, tempgpu, 0, dof_u*k );
      }

      //***********************************************************************
      // A,A block
      //***********************************************************************
      // I need to test the residual of the vector potential equation against ta*(w·∇b)
      // dtA term -------------------------------------------------------------
      Vector gAw(dof_a);
      gshape_a.Mult( wEval, gAw );
      scale = ta * ip.weight;
  // #ifndef MULT_BY_DT
  //     scale*= 1./_dt;
  // #endif
      AddMult_a_VWt(scale, gAw, shape_a, *(elmats(3,3)) );



      // This requires some more thought: I need to both extract a "mass" matrix, and to include 
      //  he extra linearisation term using < (w^{n+1}-w^{n}),(u·∇)v > 
      // if ( _includeTestLin ){
      //   // I need to test the residual of the vector potential equation against tu*(u·∇v)
      //   // dtw term -------------------------------------------------------------
      //   scale = tu * ip.weight;
      //   Vector uEval(dim);
      //   uCoeff.MultTranspose(shape_u, uEval);  // u

      //   for ( int k = 0; k < dim; ++k ){
      //     Vector tempgvi(dof_u);
      //     gshape_u.GetColumn(k,tempgvi);
      //     DenseMatrix tempugv(dof_u,dof_u);
      //     MultVWt(tempgvi, shape_u, tempugv);
      //     for ( int j = 0; j < dim; ++j ){
      //       elmats(0,0)->AddMatrix( scale*uEval(j), tempugv, dof_u*j, dof_u*k );
      //     }
      //   }
      // }

      // if ( _includeTestLin ){
      //   //***********************************************************************
      //   // A,u block
      //   //***********************************************************************
      //   // I need to test the residual of the vector potential equation against ta*(u·∇b)
      //   // dtC term -------------------------------------------------------------
      //   double AEval = (elfun[3])->operator*(shape_a); // A
      //   scale = ta * ip.weight;
      //   for (int k = 0; k < dim; k++){
      //     Vector tempugbi(dof_p);
      //     gshape_a.GetColumn(k,tempugbi);
      //     DenseMatrix tempugb(dof_a,dof_u);
      //     MultVWt(tempugbi, shape_u, tempugb);
      //     elmats(3,0)->AddMatrix( scale*AEval, tempugb, 0, dof_u*k );
      //   }
      // }


    }
  }


  // multiply everything by -1.
  for ( int i = 0; i < 4; ++i ){
    for ( int j = 0; j < 4; ++j ){
      elmats(i,j)->Neg();
    }
  }

}













void IncompressibleMHD2DTimeIntegrator::AssembleElementVector(
   const Array<const FiniteElement *> &el,
   ElementTransformation &Tr,
   const Array<const Vector *> &elfun,
   const Array<Vector *> &elvecs){

  int dof_u = el[0]->GetDof();
  int dof_p = el[1]->GetDof();
  int dof_z = el[2]->GetDof();
  int dof_a = el[3]->GetDof();

  int dim = el[0]->GetDim();

  elvecs[0]->SetSize(dof_u*dim);    *(elvecs[0]) = 0.;
  elvecs[1]->SetSize(dof_p    );    *(elvecs[1]) = 0.;
  elvecs[2]->SetSize(dof_z    );    *(elvecs[2]) = 0.;
  elvecs[3]->SetSize(dof_a    );    *(elvecs[3]) = 0.;

  shape_u.SetSize(dof_u);
  shape_a.SetSize(dof_a);

  uCoeff.UseExternalData((elfun[0])->GetData(), dof_u, dim);  // coefficients of basis functions for u


  const IntegrationRule& ir = GetRule(el, Tr);

  for (int i = 0; i < ir.GetNPoints(); ++i){
    // compute quantities specific to this integration point
    // - Jacobian-related
    const IntegrationPoint &ip = ir.IntPoint(i);
    Tr.SetIntPoint(&ip);
    const DenseMatrix adJ( Tr.AdjugateJacobian() );
    const double detJ = Tr.Weight();
    // - basis functions evaluations (on reference element)
    el[0]->CalcShape(  ip, shape_u  );
    el[3]->CalcShape(  ip, shape_a  );
    Vector uEval(dim);
    uCoeff.MultTranspose(shape_u, uEval);          // u
    double AEval = (elfun[3])->operator*(shape_a); // A



    //***********************************************************************
    // u,u block
    //***********************************************************************
    // Mass (eventually rescaled by 1/dt) -----------------------------------
    double scale = ip.weight * detJ;
// #ifndef MULT_BY_DT
//     scale*= 1./_dt;
// #endif
    // -- this contribution is made block-wise: loop over physical dimensions
    for (int k = 0; k < dim; k++){
      for ( int j = 0; j < dof_u; ++j ){
        elvecs[0]->operator()(j+k*dof_u) += shape_u(j) * scale * uEval(k);
      }
    }


    //***********************************************************************
    // A,A block
    //***********************************************************************
    // Mass (eventually rescaled by 1/dt) -----------------------------------
    scale = ip.weight * detJ;
// #ifndef MULT_BY_DT
//     scale *= 1./_dt;
// #endif
    elvecs[3]->Add( scale*AEval, shape_a );




    //***********************************************************************
    //***********************************************************************
    // IF STABILIZATION IS INCLUDED, I NEED EXTRA TERMS!
    //***********************************************************************
    //***********************************************************************
    if ( _stab ){
      // - more basis functions gradient evaluations (on ref element)
      shape_Du.SetSize(dof_u, dim);
      shape_Dp.SetSize(dof_p, dim);
      shape_Da.SetSize(dof_a, dim);

      gshape_u.SetSize(dof_u, dim);
      gshape_p.SetSize(dof_p, dim);
      gshape_a.SetSize(dof_a, dim);

      el[0]->CalcDShape( ip, shape_Du );
      el[1]->CalcDShape( ip, shape_Dp );
      el[3]->CalcDShape( ip, shape_Da );
      // - basis functions gradient evaluations (on physical element)
      Mult(shape_Du, adJ, gshape_u);  // NB: this way they are all multiplied by detJ already!
      Mult(shape_Dp, adJ, gshape_p);
      Mult(shape_Da, adJ, gshape_a);
      // - function evaluations
      Vector wEval(dim), gcEval(dim);
      _w->Eval(wEval, Tr, ip);
      _c->GetGridFunction()->GetGradient( Tr, gcEval );

      double tu=0., ta=0.;
      IncompressibleMHD2DSpaceIntegrator::GetTaus( wEval, gcEval, Tr.InverseJacobian(), _dt, _mu, _mu0, _eta, tu, ta );

      double scale =0.;



      //***********************************************************************
      // u block
      //***********************************************************************
      Vector tempu(dof_u);
      // I need to test the residual of the momentum equation against tu*(w·∇)v
      gshape_u.Mult( wEval, tempu ); // this is (w·∇)v
      // dtu ------------------------------------------------------------------
      scale = tu * ip.weight;
  //   #ifndef MULT_BY_DT
  //       scale*= 1./_dt;
  //   #endif
      // -- this contribution is made block-wise: loop over physical dimensions
      for (int k = 0; k < dim; k++){
        for ( int j = 0; j < dof_u; ++j ){
          elvecs[0]->operator()(j+k*dof_u) += tempu(j) * scale * uEval(k);
        }
      }

      //***********************************************************************
      // p block
      //***********************************************************************
      // I need to test the residual of the momentum equation against tu*∇q ***
      Vector tempp(dof_p);
      // dtu ------------------------------------------------------------------
      scale = tu * ip.weight;
  // #ifndef MULT_BY_DT
  //       scale *= 1/_dt;
  // #endif
      gshape_p.Mult( uEval,  tempp );
      elvecs[1]->Add( scale, tempp );



      //***********************************************************************
      // A block
      //***********************************************************************
      // I need to test the residual of the vector potential equation against ta*w·∇b
      Vector tempAA(dof_a);
      gshape_a.Mult( wEval, tempAA); // this is w·∇b
      // dtA ------------------------------------------------------------------
      scale = ta * ip.weight;
  // #ifndef MULT_BY_DT
  //       scale *= 1/_dt;
  // #endif
      elvecs[3]->Add( scale*AEval, tempAA );

    }
  }


  // multiply everything by -1.
  for ( int i = 0; i < 4; ++i ){
    elvecs[i]->Neg();
  }


}


















const IntegrationRule& IncompressibleMHD2DTimeIntegrator::GetRule(const Array<const FiniteElement *> &el,
                                                                      ElementTransformation &Tr){

  // Find an integration rule which is accurate enough
  const int ordU  = el[0]->GetOrder();
  const int ordA  = el[3]->GetOrder();
  const int ordGU = Tr.OrderGrad(el[0]);
  const int ordGP = Tr.OrderGrad(el[1]);
  const int ordGA = Tr.OrderGrad(el[3]);

  // I check every term in the equations, see which poly order is reached, and take the max
  Array<int> ords(3);
  // if ( _stab ){
  //   ords[0] = 2*ordU; // ( u, v )
  //   ords[1] = 2*ordA; // ( A, B  )    
  //   ords[2] = 0;
  // }else{
    ords[0] = 2*ordU + ordGU;        // ( u, (w·∇)v )
    ords[1] =   ordU + ordGP;        // ( u,    ∇q  )
    ords[2] =   ordU + ordGA + ordA; // ( A,  w·∇B  )    
  // }


  // std::cout<<"Selecting integrator of order "<<ords.Max()<<std::endl;

  // TODO: this is overkill. I should prescribe different accuracies for each component of the integrator!
  return IntRules.Get( el[0]->GetGeomType(), ords.Max() );

}





} // namespace mfem