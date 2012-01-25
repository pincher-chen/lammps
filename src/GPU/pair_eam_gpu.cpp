/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors: Trung Dac Nguyen (ORNL), W. Michael Brown (ORNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_eam_gpu.h"
#include "atom.h"
#include "force.h"
#include "comm.h"
#include "domain.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"
#include "neigh_request.h"
#include "gpu_extra.h"

using namespace LAMMPS_NS;

#define MAXLINE 1024

// External functions from cuda library for atom decomposition

int eam_gpu_init(const int ntypes, double host_cutforcesq,
		 int **host_type2rhor, int **host_type2z2r,
                 int *host_type2frho, double ***host_rhor_spline,
		 double ***host_z2r_spline, double ***host_frho_spline,
                 double rdr, double rdrho, int nrhor, int nrho, int nz2r,
		 int nfrho, int nr, const int nlocal, const int nall,
		 const int max_nbors, const int maxspecial, 
		 const double cell_size, int &gpu_mode, FILE *screen, 
		 int &fp_size);
void eam_gpu_clear();
int** eam_gpu_compute_n(const int ago, const int inum_full, const int nall,
			double **host_x, int *host_type, double *sublo,
			double *subhi, int *tag, int **nspecial, int **special,
			const bool eflag, const bool vflag, const bool eatom,
			const bool vatom, int &host_start, int **ilist,
			int **jnum,  const double cpu_time, bool &success,
			int &inum, void **fp_ptr);
void eam_gpu_compute(const int ago, const int inum_full, const int nlocal, 
		     const int nall,double **host_x, int *host_type, 
		     int *ilist, int *numj, int **firstneigh, 
		     const bool eflag, const bool vflag,
		     const bool eatom, const bool vatom, int &host_start,
		     const double cpu_time, bool &success, void **fp_ptr);
void eam_gpu_compute_force(int *ilist, const bool eflag, const bool vflag,
			   const bool eatom, const bool vatom);
double eam_gpu_bytes();

/* ---------------------------------------------------------------------- */

PairEAMGPU::PairEAMGPU(LAMMPS *lmp) : PairEAM(lmp), gpu_mode(GPU_FORCE)
{
  respa_enable = 0;
  cpu_time = 0.0;
  GPU_EXTRA::gpu_ready(lmp->modify, lmp->error); 
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

PairEAMGPU::~PairEAMGPU()
{
  eam_gpu_clear();
}

/* ---------------------------------------------------------------------- */

double PairEAMGPU::memory_usage()
{
  double bytes = Pair::memory_usage();
  return bytes + eam_gpu_bytes();
}

/* ---------------------------------------------------------------------- */

void PairEAMGPU::compute(int eflag, int vflag)
{
  int i,j,ii,jj,m,jnum,itype,jtype;
  double evdwl,*coeff;

  evdwl = 0.0;
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = eflag_global = eflag_atom = 0;
 
  int nlocal = atom->nlocal;
  int newton_pair = force->newton_pair;

  // compute density on each atom on GPU

  int nall = atom->nlocal + atom->nghost;  
  int inum, host_start, inum_dev;
  
  bool success = true;
  int *ilist, *numneigh, **firstneigh; 
  if (gpu_mode != GPU_FORCE) { 
    inum = atom->nlocal;
    firstneigh = eam_gpu_compute_n(neighbor->ago, inum, nall, atom->x,
				   atom->type, domain->sublo, domain->subhi,
				   atom->tag, atom->nspecial, atom->special,
				   eflag, vflag, eflag_atom, vflag_atom,
				   host_start, &ilist, &numneigh, cpu_time,
				   success, inum_dev, &fp_pinned);
  } else { // gpu_mode == GPU_FORCE
    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;
    eam_gpu_compute(neighbor->ago, inum, nlocal, nall, atom->x, atom->type,
		    ilist, numneigh, firstneigh, eflag, vflag, eflag_atom,
		    vflag_atom, host_start, cpu_time, success, &fp_pinned);
  }
    
  if (!success)
    error->one(FLERR,"Out of memory on GPGPU");

  // communicate derivative of embedding function

  comm->forward_comm_pair(this);
    
  // compute forces on each atom on GPU
  if (gpu_mode != GPU_FORCE) 
    eam_gpu_compute_force(NULL, eflag, vflag, eflag_atom, vflag_atom);
  else
    eam_gpu_compute_force(ilist, eflag, vflag, eflag_atom, vflag_atom);
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairEAMGPU::init_style()
{
  if (force->newton_pair) 
    error->all(FLERR,"Cannot use newton pair with eam/gpu pair style");
  
  if (!allocated) error->all(FLERR,"Not allocate memory eam/gpu pair style");
  
  // convert read-in file(s) to arrays and spline them

  file2array();
  array2spline();
  
  // Repeat cutsq calculation because done after call to init_style
  double maxcut = -1.0;
  double cut;
  for (int i = 1; i <= atom->ntypes; i++) {
    for (int j = i; j <= atom->ntypes; j++) {
      if (setflag[i][j] != 0 || (setflag[i][i] != 0 && setflag[j][j] != 0)) {
        cut = init_one(i,j);
        cut *= cut;
        if (cut > maxcut)
          maxcut = cut;
        cutsq[i][j] = cutsq[j][i] = cut;
      } else
        cutsq[i][j] = cutsq[j][i] = 0.0;
    }
  }
  double cell_size = sqrt(maxcut) + neighbor->skin;
  
  int maxspecial=0;
  if (atom->molecular)
    maxspecial=atom->maxspecial;
  int fp_size;
  int success = eam_gpu_init(atom->ntypes+1, cutforcesq, type2rhor, type2z2r,
			     type2frho, rhor_spline, z2r_spline, frho_spline,
			     rdr, rdrho, nrhor, nrho, nz2r, nfrho, nr,
			     atom->nlocal, atom->nlocal+atom->nghost, 300,
			     maxspecial, cell_size, gpu_mode, screen, fp_size);
  GPU_EXTRA::check_flag(success,error,world);
  
  if (gpu_mode == GPU_FORCE) {
    int irequest = neighbor->request(this);
    neighbor->requests[irequest]->half = 0;
    neighbor->requests[irequest]->full = 1;
  }

  if (fp_size == sizeof(double))
    fp_single = false;
  else
    fp_single = true;
}

/* ---------------------------------------------------------------------- */

int PairEAMGPU::pack_comm(int n, int *list, double *buf, int pbc_flag, 
			  int *pbc)
{
  int i,j,m;

  m = 0;

  if (fp_single) {
    float *fp_ptr = (float *)fp_pinned;
    for (i = 0; i < n; i++) {
      j = list[i];
      buf[m++] = static_cast<double>(fp_ptr[j]);
    }
  } else {
    double *fp_ptr = (double *)fp_pinned;
    for (i = 0; i < n; i++) {
      j = list[i];
      buf[m++] = fp_ptr[j];
    }
  }

  return 1;
}

/* ---------------------------------------------------------------------- */

void PairEAMGPU::unpack_comm(int n, int first, double *buf)
{
  int i,m,last;

  m = 0;
  last = first + n;
  if (fp_single) {
    float *fp_ptr = (float *)fp_pinned;
    for (i = first; i < last; i++) fp_ptr[i] = buf[m++];
  } else {
    double *fp_ptr = (double *)fp_pinned;
    for (i = first; i < last; i++) fp_ptr[i] = buf[m++];
  }
}
