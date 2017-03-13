#ifndef DMESH_BLOCK_H
#define DMESH_BLOCK_H

#include "cuda_funcs.h"
#include <vector>

class dMeshBlock
{
public:
  /* ------ Variables related to grid connectivity ------ */

  int nnodes;  //! Number of nodes in grid
  int ncells;  //! Number of cells/elements in grid
  int nc_adt;  //! Number of cells/elements in grid
  int nfaces;  //! Number of faces in grid (used for Art. Bnd.)
  int ntypes;  //! Number of different cell types present in grid
  int nftype;  //! Number of different face types present in grid
  int *nv;     //! Number of vertices for each type of cell
  int *nc;     //! Number of cells of each cell type
  int *nf;     //! Number of faces for each cell type
  int *nfv;    //! Number of vertices per face for each face type (3 or 4)
  int nobc;    //! Number of overset boundary nodes
  int nwbc;    //! Number of wall boundary nodes

  int** c2v;   //! Cell-to-vertex connectivity
  int** f2v;   //! Face-to-vertex connectivity
  int* f2c;    //! Face-to-cell connectivity
  int* c2f;    //! Cell-to-face connectivity
  int* wNodes; //! List of nodes on wall boundaries
  int* oNodes; //! List of nodes on pre-defined overset boundaries

  double *x;      //! Grid nodes coordinates [nnodes * ndim]
  double *coord;  //! Element node coordinates [ncells * nvert * ndim]

  int nDims = 3;

  int nvert;

  /* ------ Variables related to overset blanking ------ */

  dvec<int> iblank;
  int* iblank_cell;
  int* iblank_face;

  /* ------ Variables related to search operations ------ */

  dvec<int> eleList;     //! List of elements in d/ADT
  dvec<double> eleBBox;  //! Bounding box of elements in d/ADT

  int nsearch;
  int donorCount;
  dvec<int> isearch;
  dvec<double> xsearch;
  dvec<double> rst;
  dvec<int> donorId;

  cudaStream_t stream;

  dvec<int> ijk2gmsh;
  dvec<double> xlist;

  bool rrot = false;
  dvec<double> Rmat, offset;

  /* ------ Member Functions ------ */

  dMeshBlock() { }

  void dataToDevice(int ndims, int nnodes, int ncells, int ncells_adt, int nsearch, int* nv,
      int* nc, int* eleList, double* eleBBox, int* isearch, double* xsearch);

  void setDeviceData(double* vx, double* ex, int* ibc, int* ibf);

  void setTransform(double *mat, double *off, int ndim);

  void updateSearchPoints(int nsearch, int* isearch, double* xsearch);

  template<int ndim, int nside>
  __device__
  void checkContainment(int adtEle, int& cellID, const double* __restrict__ bbox,
      const double* __restrict__ xsearch, double* __restrict__ rst);

  template<int nSide>
  __device__ __forceinline__
  void calcDShape(double* __restrict__ shape,
      double* __restrict__ dshape, const double* loc);

  template<int nSide>
  __device__ __forceinline__
  bool getRefLoc(const double* __restrict__ coords, const double* __restrict__ bbox,
                 const double* __restrict__ xyz, double* __restrict__ rst);
};

template<int nSide>
__device__ __forceinline__
void dMeshBlock::calcDShape(double* __restrict__ shape, double* __restrict__ dshape,
                            const double* loc)
{
  double xi = loc[0];
  double eta = loc[1];
  double mu = loc[2];

  double lag_i[nSide];
  double lag_j[nSide];
  double lag_k[nSide];
  double dlag_i[nSide];
  double dlag_j[nSide];
  double dlag_k[nSide];

  for (int i = 0; i < nSide; i++)
  {
    lag_i[i] = cuda_funcs::Lagrange_gpu(xlist.data(), nSide,  xi, i);
    lag_j[i] = cuda_funcs::Lagrange_gpu(xlist.data(), nSide, eta, i);
    lag_k[i] = cuda_funcs::Lagrange_gpu(xlist.data(), nSide,  mu, i);
    dlag_i[i] = cuda_funcs::dLagrange_gpu(xlist.data(), nSide,  xi, i);
    dlag_j[i] = cuda_funcs::dLagrange_gpu(xlist.data(), nSide, eta, i);
    dlag_k[i] = cuda_funcs::dLagrange_gpu(xlist.data(), nSide,  mu, i);
  }

  //int nd = 0;
  for (int k = 0; k < nSide; k++)
    for (int j = 0; j < nSide; j++)
      for (int i = 0; i < nSide; i++)
      {
        int gnd = ijk2gmsh[i+nSide*(j+nSide*k)];
        //int gnd = i+nSide*(j+nSide*k);
        shape[gnd] = lag_i[i] * lag_j[j] * lag_k[k];
        dshape[gnd*3+0] = dlag_i[i] *  lag_j[j] *  lag_k[k];
        dshape[gnd*3+1] =  lag_i[i] * dlag_j[j] *  lag_k[k];
        dshape[gnd*3+2] =  lag_i[i] *  lag_j[j] * dlag_k[k];
        //nd++;
      }
}

template<int nSide>
__device__
bool dMeshBlock::getRefLoc(const double* __restrict__ coords,
    const double* __restrict__ bbox, const double* __restrict__ xyz,
    double* __restrict__ rst)
{
  const int nNodes = nSide*nSide*nSide;

  // Use a relative tolerance to handle extreme grids
  double h = fmin(bbox[3]-bbox[0],bbox[4]-bbox[1]);
  h = fmin(h,bbox[5]-bbox[2]);

  double tol = 1e-12*h;

  int iter = 0;
  int iterMax = 10;
  double norm = 1;
  double norm_prev = 2;

  double shape[nNodes];
  double dshape[3*nNodes];

  rst[0] = 0.;
  rst[1] = 0.;
  rst[2] = 0.;

  while (norm > tol && iter < iterMax)
  {
    calcDShape<nSide>(shape, dshape, rst);

    double dx[3] = {xyz[0], xyz[1], xyz[2]};
    double grad[3][3] = {{0.0}};
    double ginv[3][3];

    for (int nd = 0; nd < nNodes; nd++)
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          grad[i][j] += coords[i+3*nd] * dshape[nd*3+j];

    for (int nd = 0; nd < nNodes; nd++)
      for (int i = 0; i < 3; i++)
        dx[i] -= shape[nd] * coords[i+3*nd];

    double detJ = cuda_funcs::det_3x3(&grad[0][0]);

    cuda_funcs::adjoint_3x3(&grad[0][0], &ginv[0][0]);

    double delta[3] = {0.0};
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        delta[i] += ginv[i][j]*dx[j]/detJ;

    norm = sqrt(dx[0]*dx[0]+dx[1]*dx[1]+dx[2]*dx[2]);
    for (int i = 0; i < 3; i++)
      rst[i] = max(min(rst[i]+delta[i],1.),-1.);

    if (iter > 1 && norm > .99*norm_prev) // If it's clear we're not converging
      break;

    norm_prev = norm;

    iter++;
  }

  if (norm <= tol)
    return true;
  else
    return false;
}

template<int ndim, int nside>
__device__
void dMeshBlock::checkContainment(int adtEle, int& cellID, const double* __restrict__ bbox,
    const double* __restrict__ xyz, double* __restrict__ rst)
{
  //const int ndim_adt = 2*ndim;
  const int nNodes = nside*nside*nside;

  int ele = eleList[adtEle];
  cellID = -1;

  double ecoord[nNodes*ndim];
  for (int i = 0; i < nNodes; i++)
    for (int d = 0; d < ndim; d++)
      ecoord[i*ndim+d] = coord[ele+ncells*(d+ndim*i)];
  //ecoord[i*ndim+d] = coord[d+ndim*(i+nNodes*ele)];

  bool isInEle = false;

  if (rrot) // Transform search point back to current physical location
  {
    double x2[ndim];
    for (int d = 0; d < ndim; d++)
      x2[d] = xyz[d] + offset[d];

    isInEle = getRefLoc<nside>(ecoord,bbox,x2,rst);
  }
  else
  {
    isInEle = getRefLoc<nside>(ecoord,bbox,xyz,rst);
  }


  if (isInEle) cellID = ele;
}

#endif