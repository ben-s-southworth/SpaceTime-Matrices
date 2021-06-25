#include "parblocklowtrioperator.hpp"

namespace mfem{

ParBlockLowTriOperator::ParBlockLowTriOperator( const MPI_Comm& comm, const ParBlockView& view ):
  _comm(comm), _view(view){

  MPI_Comm_size( comm, &_numProcs );
  MPI_Comm_rank( comm, &_myRank );

  _ownsOps.SetSize( _numProcs, false );
  _ops.SetSize(     _numProcs, NULL );

}



// Operator application
void ParBlockLowTriOperator::Mult (const Vector & x, Vector & y) const{

  MFEM_ASSERT( width  == 0 || x.Size() == width,  "incorrect  input Vector size");
  MFEM_ASSERT( height == 0 || y.Size() == height, "incorrect output Vector size");

  // treat first diagonal differently (no communication needed)
  if ( _ops[0] ){
    _ops[0]->Mult(x,y);
  }else{
    y = 0.;
  }

  // for all other diagonals
  for ( int i=1; i<_numProcs; ++i ){

    if ( _ops[i] != NULL ){
      if ( _view == COL_VIEW ){
        // Here you have a choice. This is due to the fact that there are two ways to "see" this operator
        //  1- a col-oriented view, where each processor owns operators on the same col
        //      In this case, every processor does the mult, and sends the result
        if ( _myRank < _numProcs-i ){
          Vector tmp(height);
          _ops[i]->Mult( x, tmp );
          MPI_Send( tmp.GetData(), height, MPI_DOUBLE, _myRank+i, _numProcs*_myRank+i,     _comm );
        }
        if ( _myRank > i-1 ){
          Vector tmp(height);
          MPI_Recv( tmp.GetData(), height, MPI_DOUBLE, _myRank-i, _numProcs*(_myRank-i)+i, _comm, MPI_STATUS_IGNORE );
          y += tmp;
        }
      }else{
        //  2- a row-oriented view, where each processor owns operators on the same row:
        //      In this case, every processor sends the input, and the receiving proc does the mult
        if ( _myRank < _numProcs-i ){
          MPI_Send(     x.GetData(), width, MPI_DOUBLE, _myRank+i, _numProcs*_myRank+i,     _comm );
        }
        if ( _myRank > i-1 ){
          Vector tmp(height), prevX(width);
          MPI_Recv( prevX.GetData(), width, MPI_DOUBLE, _myRank-i, _numProcs*(_myRank-i)+i, _comm, MPI_STATUS_IGNORE );
          _ops[i]->Mult( prevX, tmp );
          y += tmp;
        }
      }

    }
  }
}






// Operator transpose application
void ParBlockLowTriOperator::MultTranspose(const Vector & x, Vector & y) const{

  MFEM_ASSERT( height == 0 || x.Size() == height, "incorrect  input Vector size");
  MFEM_ASSERT( width  == 0 || y.Size() == width,  "incorrect output Vector size");

  // treat first diagonal differently (no communication needed)
  if ( _ops[0] ){
    _ops[0]->MultTranspose(x,y);
  }else{
    y = 0.;
  }

  // for all other diagonals
  for ( int i=1; i<_numProcs; ++i ){
    if ( _ops[i] != NULL ){
      // The choice we had for the "view" of this operator is now flipped, since we are considering
      //  multiplication by *transpose*
      if ( _view == COL_VIEW ){
        // 1- a col-oriented view, where each processor owns operators on the same col
        //  In this case, for transposition, every processor sends the input, and the receiving proc does the mult
        if ( _myRank > i-1 ){
          MPI_Send(     x.GetData(), height, MPI_DOUBLE, _myRank-i, _numProcs*_myRank-i,     _comm );
        }
        if ( _myRank < _numProcs-i ){
          Vector tmp(width), postX(height);
          MPI_Recv( postX.GetData(), height, MPI_DOUBLE, _myRank+i, _numProcs*(_myRank+i)-i, _comm, MPI_STATUS_IGNORE );
          _ops[i]->MultTranspose( postX, tmp );
          y += tmp;
        }
      }else{
        // 2- a row-oriented view, where each processor owns operators on the same row:
        //   In this case, after transposition, every processor does the mult, and sends the result
        if ( _myRank > i-1 ){
          Vector tmp(height);
          _ops[i]->MultTranspose( x, tmp );
          MPI_Send( tmp.GetData(), height, MPI_DOUBLE, _myRank-i, _numProcs*_myRank-i,     _comm );
        }
        if ( _myRank < _numProcs-i ){
          Vector tmp(height);
          MPI_Recv( tmp.GetData(), height, MPI_DOUBLE, _myRank+i, _numProcs*(_myRank+i)-i, _comm, MPI_STATUS_IGNORE );
          y += tmp;
        }
      }

    }
  }
}






void ParBlockLowTriOperator::SetBlockDiag( const SparseMatrix * op, int i, bool own ){
  MFEM_ASSERT( i < _numProcs, "Invalid diagonal index" );
  MFEM_ASSERT( op==NULL || ( ( height == 0 || height == op->Height() )
                          && ( width  == 0 || width  == op->Width()  ) ), "Invalid operator size" );

  if ( _ownsOps[i] ){
    delete _ops[i];
  }

  _ops[i] = op;
  _ownsOps[i] = own;
  
  if ( op!=NULL ){
    height = op->Height();
    width  = op->Width();
  }else{
    height = 0;
    width  = 0;
  }

}


const Operator* ParBlockLowTriOperator::GetBlockDiag( int i ){
  if ( i>=_numProcs ){
    return NULL;
  }else{
    return _ops[i];
  }

}






ParBlockLowTriOperator::~ParBlockLowTriOperator(){
  for (int i=0; i < _numProcs; ++i){
    if ( _ownsOps[i] ){
      delete _ops[i];
    }
  }
}








} // mfem namespace