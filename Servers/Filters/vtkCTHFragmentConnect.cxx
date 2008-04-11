/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkCTHFragmentConnect.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkCTHFragmentConnect.h"

#include "vtkMultiProcessController.h"

#include "vtkCollection.h"
#include "vtkCellArray.h"
#include "vtkCommand.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMarchingCubesCases.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkCellData.h"
#include "vtkDataSetWriter.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkUniformGrid.h"
#include "vtkAMRBox.h"
#include <math.h>
#include "vtkDoubleArray.h"

#include "vtkDataSet.h"
#include "vtkDataObject.h"
#include "vtkHierarchicalBoxDataSet.h"
#include "vtkXMLPolyDataWriter.h"

// 0 is not visited, positive is an actual ID.
#define PARTICLE_CONNECT_EMPTY_ID -1

vtkCxxRevisionMacro(vtkCTHFragmentConnect, "1.15");
vtkStandardNewMacro(vtkCTHFragmentConnect);

//============================================================================
// A class that implements an equivalent set.  It is used to combine fragments
// from different processes.
//
// I believe that this class is a strictly ordered tree of equivalences.
// Every member points to its own id or an id smaller than itself.
class vtkCTHFragmentEquivalenceSet
{
public:
  vtkCTHFragmentEquivalenceSet();
  ~vtkCTHFragmentEquivalenceSet();

  void Print();

  void Initialize();
  void AddEquivalence(int id1, int id2);

  // The length of the equivalent array...
  int GetNumberOfMembers() { return this->EquivalenceArray->GetNumberOfTuples();}

  // Return the id of the equivalent set.
  int GetEquivalentSetId(int memberId);

  // Equivalent set ids are reassinged to be sequential.
  // You cannot add anymore equivalences after this is called.
  int ResolveEquivalences();

  void DeepCopy(vtkCTHFragmentEquivalenceSet* in);

  // Needed for seding the set over MPI.
  // Be very careful with the pointer.
  int* GetPointer() { return this->EquivalenceArray->GetPointer(0);}

  // We should fix the pointer API and hide this ivar.
  int Resolved;

private:

  // To merge connected framgments that have different ids because they were
  // traversed by different processes or passes.
  vtkIntArray *EquivalenceArray;

  // Return the id of the equivalent set.
  int GetReference(int memberId);
  void EquateInternal(int id1, int id2);
};

//----------------------------------------------------------------------------
vtkCTHFragmentEquivalenceSet::vtkCTHFragmentEquivalenceSet()
{
  this->Resolved = 0;
  this->EquivalenceArray = vtkIntArray::New();
}

//----------------------------------------------------------------------------
vtkCTHFragmentEquivalenceSet::~vtkCTHFragmentEquivalenceSet()
{
  this->Resolved = 0;
  if (this->EquivalenceArray)
    {
    this->EquivalenceArray->Delete();
    this->EquivalenceArray = 0;
    }
}

//----------------------------------------------------------------------------
void vtkCTHFragmentEquivalenceSet::Initialize()
{
  this->Resolved = 0;
  this->EquivalenceArray->Initialize();
}

//----------------------------------------------------------------------------
void vtkCTHFragmentEquivalenceSet::DeepCopy(vtkCTHFragmentEquivalenceSet* in)
{
  this->Resolved = in->Resolved;
  this->EquivalenceArray->DeepCopy(in->EquivalenceArray);
}

//----------------------------------------------------------------------------
// Return the id of the equivalent set.
void vtkCTHFragmentEquivalenceSet::Print()
{
  int num = this->GetNumberOfMembers();
  cerr << num << endl;
  for (int ii = 0; ii < num; ++ii)
    {
    cerr << "  " << ii << " : " << this->GetEquivalentSetId(ii) << endl;
    }
  cerr << endl;
}


//----------------------------------------------------------------------------
// Return the id of the equivalent set.
int vtkCTHFragmentEquivalenceSet::GetEquivalentSetId(int memberId)
{
  int ref;
  
  ref = this->GetReference(memberId);
  while (!this->Resolved && ref != memberId)
    {
    memberId = ref;
    ref = this->GetReference(memberId);
    }
    
  return ref;
}

//----------------------------------------------------------------------------
// Return the id of the equivalent set.
int vtkCTHFragmentEquivalenceSet::GetReference(int memberId)
{
  if (memberId >= this->EquivalenceArray->GetNumberOfTuples())
    { // We might consider this an error ...
    return memberId;
    }
  return this->EquivalenceArray->GetValue(memberId);
}

//----------------------------------------------------------------------------
// Makes two new or existing ids equivalent.
// If the array is too small, the range of ids is increased until it contains
// both the ids.  Negative ids are not allowed.
void vtkCTHFragmentEquivalenceSet::AddEquivalence(int id1, int id2)
{
  if (this->Resolved)
    {
    cerr << "Set already resolved.  you cannot add more equivalences.\n";
    return;
    }
    
  int num = this->EquivalenceArray->GetNumberOfTuples();

  // Expand the range to include both ids.
  while (num <= id1 || num <= id2)
    {
    // All values inserted are equivalent to only themselves.
    this->EquivalenceArray->InsertNextTuple1(num);
    ++num;
    }

 // Our rule for references 9in the equivalent set is that
 // all elements must point to a member equal to or smaller
 // than itself.
 
 // The only problem we could encounter is changing a reference.
 // we do not want to orphan anything previously referenced.

  // Replace the larger references.
  if (id1 < id2)
    {
    // We could follow the references to the smallest.  It might
    // make this processing more efficient.  This is a compromise.
    // The referenced id will always be smaller than the id so
    // order does not change.
    this->EquateInternal(this->GetReference(id1), id2);
    }
  else
    {
    this->EquateInternal(this->GetReference(id2), id1);
    }
}

//----------------------------------------------------------------------------
// id1 must be less than or equal to id2.
void vtkCTHFragmentEquivalenceSet::EquateInternal(int id1, int id2)
{
  // This is the reference that might be orphaned in this process.
  int oldRef = this->GetEquivalentSetId(id2);
  
 // The only problem we could encounter is changing a reference.
 // we do not want to orphan anything previously referenced.
  if (oldRef == id2 || oldRef == id1)
    {
    this->EquivalenceArray->SetValue(id2, id1);
    }
  else if (oldRef > id1)
    {
    this->EquivalenceArray->SetValue(id2, id1);
    this->EquateInternal(id1, oldRef);
    }
  else
    { // oldRef < id1
    this->EquateInternal(oldRef, id1);
    }
}


//----------------------------------------------------------------------------
// Returns the number of merged sets.
int vtkCTHFragmentEquivalenceSet::ResolveEquivalences()
{
  // Go through the equivalence array collapsing chains 
  // and assigning consecutive ids.
  
  int count = 0;
  int id;
  int newId;
  
  int numIds = this->EquivalenceArray->GetNumberOfTuples();
  for (int ii = 0; ii < numIds; ++ii)
    {
    id = this->EquivalenceArray->GetValue(ii);
    if (id == ii)
      { // This is a new equivalence set.
      this->EquivalenceArray->SetValue(ii, count);
      ++count;
      }
    else
      {
      // All earlier ids will be resolved already.
      // This array only point to less than or equal ids. (id <= ii).
      newId = this->EquivalenceArray->GetValue(id);
      this->EquivalenceArray->SetValue(ii,newId);
      }
    }
  this->Resolved = 1;
  cerr << "Final number of equivalent sets: " << count << endl;

  return count;
}



//============================================================================
// A class to hold block information. Input is not HierarichalBox so I need
// to extract and store this extra information for each block.
class vtkCTHFragmentConnectBlock
{
public:
  vtkCTHFragmentConnectBlock();
  ~vtkCTHFragmentConnectBlock();
  
  // For normal (local) blocks.
  void Initialize(int blockId, vtkImageData* imageBlock, int level, 
                  double globalOrigin[3], double rootSapcing[3],
                  const char* volumeFractionArrayName);
  
  // Determine the extent without overlap using the standard block
  // dimensions.
  void ComputeBaseExtent(int blockDims[3]);
  
  // For ghost layers received by other processes.
  // The argument list here is getting a bit long ....
  void InitializeGhostLayer(unsigned char* volFraction, 
                            int cellExtent[6],
                            int level,
                            double globalOrigin[3],
                            double rootSpacing[3],
                            int ownerProcessId,
                            int blockId);
  // This adds a block as our neighbor and makes the reciprical connection too.
  // It is used to connect ghost layer blocks.  Ghost blocks need the reciprical 
  // connection to continue the connectivity search.
  // I am hoping this will make the number of fragments to merge smaller.
  void AddNeighbor(vtkCTHFragmentConnectBlock *block, int axis, int maxFlag);
  
  void GetPointExtent(int ext[6]);
  void GetCellExtent(int ext[6]);
  void GetCellIncrements(int incs[3]);
  const int*  GetCellIncrements() {return this->CellIncrements;}
  void GetBaseCellExtent(int ext[6]);
  // This was a major time consumer so use the pointer directly.
  const int* GetBaseCellExtent() { return this->BaseCellExtent;}


  unsigned char GetGhostFlag() { return this->GhostFlag;}
  // Information saved for ghost cells that makes it easier to
  // resolve equivalent fragment ids.
  int GetOwnerProcessId() { return this->ProcessId;}
  int GetBlockId() { return this->BlockId;}

  //Returns a pointer to the minimum AMR Extent with is the first
  // cell values that is not a ghost cell.
  unsigned char* GetBaseVolumeFractionPointer();
  int*           GetBaseFragmentIdPointer();
  int*           GetFragmentIdPointer() {return this->FragmentIds;}
  int            GetLevel() {return this->Level;}
  double*        GetSpacing() {return this->Spacing;}
  double*        GetOrigin() {return this->Origin;}
  // Faces are indexed: 0=xMin, 1=xMax, 2=yMin, 3=yMax, 4=zMin, 5=zMax
  int            GetNumberOfFaceNeighbors(int face)
    {return this->Neighbors[face].size();}
  vtkCTHFragmentConnectBlock* GetFaceNeighbor(int face, int neighborId)
    {return (vtkCTHFragmentConnectBlock*)(this->Neighbors[face][neighborId]);}

  // For basis of face coordinate systems.
  // -x,x,-y,y,-z,z
  double HalfEdges[6][3];

  // Just for debugging.
  int LevelBlockId;

  void ExtractExtent(unsigned char* buf, int ext[6]);

private:

  unsigned char GhostFlag;
  // Information saved for ghost cells that makes it easier to
  // resolve equivalent fragment ids.
  int BlockId;
  int ProcessId;

  // This id is for connectivity computation.
  // It is essentially a cell array of the image.
  int *FragmentIds;

  // The original image block (worry about rectilinear grids later).
  // We have two ways of managing the memory.  We only keep the 
  // image around to keep a reference.
  vtkImageData* Image;
  unsigned char* VolumeFractionArray;
  // We might as well store the increments.
  int CellIncrements[3];

  // Extent of the cell arrays as in memory.
  int CellExtent[6];
  // Useful for neighbor computations and to avoid processing overlapping cells.
  int BaseCellExtent[6];

  // The blocks do not follow the convention of sharing an origin.
  // Just save these for placing the faces.  We will have to find
  // another way to compute neighbors.
  double Spacing[3];
  double Origin[3];
  int Level;

  // Store index to neiboring blocks.
  // There may be more than one neighbor because higher levels.
    vtkstd::vector<vtkCTHFragmentConnectBlock*> Neighbors[6];

};

//----------------------------------------------------------------------------
vtkCTHFragmentConnectBlock::vtkCTHFragmentConnectBlock ()
{
  this->GhostFlag = 0;
  this->Image = 0;
  this->VolumeFractionArray = 0;
  this->Level = 0;
  this->CellIncrements[0] = this->CellIncrements[1] = this->CellIncrements[2] = 0;
  for (int ii = 0; ii < 6; ++ii)
    {
    this->CellExtent[ii] = 0;
    this->BaseCellExtent[ii] = 0;
    }
  this->FragmentIds = 0;
  this->Spacing[0] = this->Spacing[1] = this->Spacing[2] = 0.0;
  this->Origin[0] = this->Origin[1] = this->Origin[2] = 0.0;
}
//----------------------------------------------------------------------------
vtkCTHFragmentConnectBlock::~vtkCTHFragmentConnectBlock()
{
  if (this->Image)
    {
    this->VolumeFractionArray = 0;
    this->Image->UnRegister(0);
    this->Image = 0;
    }
  if (this->VolumeFractionArray)
    { // Memory was allocated without an image.
    delete [] this->VolumeFractionArray;
    this->VolumeFractionArray = 0;
    }
    
  this->Level = 0;
      
  for (int ii = 0; ii < 6; ++ii)
    {
    this->CellExtent[ii] = 0;
    this->BaseCellExtent[ii] = 0;
    }
  
  if (this->FragmentIds != 0)
    {
    delete [] this->FragmentIds;
    this->FragmentIds = 0;
    }
  this->Spacing[0] = this->Spacing[1] = this->Spacing[2] = 0.0;  
  this->Origin[0] = this->Origin[1] = this->Origin[2] = 0.0;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::Initialize(
  int blockId, 
  vtkImageData* image, 
  int level, 
  double globalOrigin[3],
  double rootSpacing[3],
  const char* volumeFractionArrayName)
{
  if (this->VolumeFractionArray)
    {
    vtkGenericWarningMacro("Block alread initialized !!!");
    return;
    }
  if (image == 0)
    {
    vtkGenericWarningMacro("No image to initialize with.");
    return;
    }
  
  this->BlockId = blockId;
  this->Image = image;
  this->Image->Register(0);
  this->Level = level;
  image->GetSpacing(this->Spacing);
  image->GetOrigin(this->Origin);

  int numCells = image->GetNumberOfCells();
  this->FragmentIds = new int[numCells];
  // Initialize fragment ids to -1 / uninitialized
  for (int ii = 0; ii < numCells; ++ii)
    {
    this->FragmentIds[ii] = -1;
    }
  int imageExt[6];
  image->GetExtent(imageExt);

  // Extract what we need form the image because we do not reference it again.
  vtkDataArray* volumeFractionArray = this->Image->GetCellData()->GetArray(volumeFractionArrayName);

  if (volumeFractionArray == 0)
    {
    vtkGenericWarningMacro("Could not find volume fraction array " << volumeFractionArrayName);
    return;
    }

  this->VolumeFractionArray = (unsigned char*)(volumeFractionArray->GetVoidPointer(0));

  // Since CTH does not use convention that all blocks share an origin,
  // We must compute a new extent to help locate neighboring blocks.
  // It is important to compute the global origin so that the base min
  // extent is a multiple of the block dimensions (0, 8, 16...).
  int shift;
  shift = (int)(((this->Origin[0] - globalOrigin[0]) / this->Spacing[0]) + 0.5); 
  this->CellExtent[0] = imageExt[0] + shift;
  this->CellExtent[1] = imageExt[1] + shift - 1;
  shift = (int)(((this->Origin[1] - globalOrigin[1]) / this->Spacing[1]) + 0.5); 
  this->CellExtent[2] = imageExt[2] + shift;
  this->CellExtent[3] = imageExt[3] + shift - 1;
  shift = (int)(((this->Origin[2] - globalOrigin[2]) / this->Spacing[2]) + 0.5); 
  this->CellExtent[4] = imageExt[4] + shift;
  this->CellExtent[5] = imageExt[5] + shift - 1;
  for (int ii = 0; ii < 3; ++ii)
    {
    this->Origin[ii] = globalOrigin[ii];
    }

  // On this pass, assume that there is no overalp.
  this->BaseCellExtent[0] = this->CellExtent[0];
  this->BaseCellExtent[1] = this->CellExtent[1];
  this->BaseCellExtent[2] = this->CellExtent[2];
  this->BaseCellExtent[3] = this->CellExtent[3];
  this->BaseCellExtent[4] = this->CellExtent[4];
  this->BaseCellExtent[5] = this->CellExtent[5];

  this->CellIncrements[0] = 1;
  // Point extent -> cell increments.  Do not add 1.
  this->CellIncrements[1] = imageExt[1]-imageExt[0]; 
  this->CellIncrements[2] = this->CellIncrements[1] * (imageExt[3]-imageExt[2]);

  // As a sanity check, compare spacing and level.
  if ((int)(rootSpacing[0] / this->Spacing[0] + 0.5) != (1<<(this->Level)))
    {
    vtkGenericWarningMacro("Spacing does not look correct for CTH AMR structure.");
    }
  if ((int)(rootSpacing[1] / this->Spacing[1] + 0.5) != (1<<(this->Level)))
    {
    vtkGenericWarningMacro("Spacing does not look correct for CTH AMR structure.");
    }
  if ((int)(rootSpacing[2] / this->Spacing[2] + 0.5) != (1<<(this->Level)))
    {
    vtkGenericWarningMacro("Spacing does not look correct for CTH AMR structure.");
    }

  // This will have to change, but I will leave it for now.
  this->HalfEdges[1][0] = this->Spacing[0]*0.5;
  this->HalfEdges[1][1] = this->HalfEdges[1][2] = 0.0;
  this->HalfEdges[3][1] = this->Spacing[1]*0.5;
  this->HalfEdges[3][0] = this->HalfEdges[3][2] = 0.0;
  this->HalfEdges[5][2] = this->Spacing[2]*0.5;
  this->HalfEdges[5][0] = this->HalfEdges[5][1] = 0.0;
  for (int ii = 0; ii < 3; ++ii)
    {
    this->HalfEdges[0][ii] = -this->HalfEdges[1][ii];
    this->HalfEdges[2][ii] = -this->HalfEdges[3][ii];
    this->HalfEdges[4][ii] = -this->HalfEdges[5][ii];
    }
}

//----------------------------------------------------------------------------
// I would really like to make subclasses of blocks,
// Memory is managed differently.
// Skip much of the initialization because ghost layers produce no surface.
// Ghost layers need half edges too.
void vtkCTHFragmentConnectBlock::ComputeBaseExtent(int blockDims[3])
{
  if (this->GhostFlag)
    {
    // This computation does not work for ghost cells.
    // InitializeGhostCell should setup the base extent properly.
    return;
    }

  // Lets store the point extent of the block without any overlap.
  // We cannot assume that all blocks have an overlap of 1.
  // because the spyplot reader strips the "invalid" overlap cells from 
  // the outer surface of the data.
  int baseDim;
  int iMin, iMax;
  int tmp;
  for (int ii = 0; ii < 3; ++ii)
    {
    baseDim = blockDims[ii];
    iMin = 2* ii;
    iMax = iMin + 1;
    // This assumes that all extents are positive.  ceil is too complicated.
    tmp = this->BaseCellExtent[iMin];
    tmp = (tmp+baseDim-1) / baseDim;
    this->BaseCellExtent[iMin] = tmp*baseDim;
    
    tmp = this->BaseCellExtent[iMax]+1;
    tmp = tmp / baseDim;
    this->BaseCellExtent[iMax] = tmp*baseDim - 1;
    }
}


//----------------------------------------------------------------------------
// I would really like to make subclasses of blocks,
// Memory is managed differently.
// Skip much of the initialization because ghost layers produce no surface.
// Ghost layers need half edges too.
void vtkCTHFragmentConnectBlock::InitializeGhostLayer(
  unsigned char* volFraction, 
  int cellExtent[6],
  int level,
  double globalOrigin[3],
  double rootSpacing[3],
  int ownerProcessId,
  int blockId)
{
  if (this->VolumeFractionArray)
    {
    vtkGenericWarningMacro("Block alread initialized !!!");
    return;
    }

  this->ProcessId = ownerProcessId;
  this->GhostFlag = 1;
  this->BlockId = blockId;

  this->Image = 0;
  this->Level = level;
  // Skip spacing and origin.

  int numCells = (cellExtent[1]-cellExtent[0]+1)
                   *(cellExtent[3]-cellExtent[2]+1)
                   *(cellExtent[5]-cellExtent[4]+1);
  this->FragmentIds = new int[numCells];
  // Initialize fragment ids to -1 / uninitialized
  for (int ii = 0; ii < numCells; ++ii)
    {
    this->FragmentIds[ii] = -1;
    }

  // Extract what we need form the image because we do not reference it again.
  this->VolumeFractionArray = new unsigned char[numCells];
  memcpy(this->VolumeFractionArray,volFraction, numCells);

  this->CellExtent[0] = cellExtent[0];
  this->CellExtent[1] = cellExtent[1];
  this->CellExtent[2] = cellExtent[2];
  this->CellExtent[3] = cellExtent[3];
  this->CellExtent[4] = cellExtent[4];
  this->CellExtent[5] = cellExtent[5];

  // No overlap in ghost layers
  this->BaseCellExtent[0] = cellExtent[0];
  this->BaseCellExtent[1] = cellExtent[1];
  this->BaseCellExtent[2] = cellExtent[2];
  this->BaseCellExtent[3] = cellExtent[3];
  this->BaseCellExtent[4] = cellExtent[4];
  this->BaseCellExtent[5] = cellExtent[5];

  this->CellIncrements[0] = 1;
  // Point extent -> cell increments.  Do not add 1.
  this->CellIncrements[1] = cellExtent[1]-cellExtent[0]+1; 
  this->CellIncrements[2] = this->CellIncrements[1] * (cellExtent[3]-cellExtent[2]+1);

  for (int ii = 0; ii < 3; ++ii)
    {
    this->Origin[ii] = globalOrigin[ii];
    this->Spacing[ii] =  rootSpacing[ii] / (double)(1<<(this->Level));
    }

  // This will have to change, but I will leave it for now.
  this->HalfEdges[1][0] = this->Spacing[0]*0.5;
  this->HalfEdges[1][1] = this->HalfEdges[1][2] = 0.0;
  this->HalfEdges[3][1] = this->Spacing[1]*0.5;
  this->HalfEdges[3][0] = this->HalfEdges[3][2] = 0.0;
  this->HalfEdges[5][2] = this->Spacing[2]*0.5;
  this->HalfEdges[5][0] = this->HalfEdges[5][1] = 0.0;
  for (int ii = 0; ii < 3; ++ii)
    {
    this->HalfEdges[0][ii] = -this->HalfEdges[1][ii];
    this->HalfEdges[2][ii] = -this->HalfEdges[3][ii];
    this->HalfEdges[4][ii] = -this->HalfEdges[5][ii];
    }
}



//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::AddNeighbor(
  vtkCTHFragmentConnectBlock *neighbor, 
  int axis, 
  int maxFlag)
{
  if (maxFlag)
    { // max neighbor
    this->Neighbors[2*axis+1].push_back(neighbor);
    neighbor->Neighbors[2*axis].push_back(this);
    }
  else
    { // min neighbor
    this->Neighbors[2*axis].push_back(neighbor);
    neighbor->Neighbors[2*axis+1].push_back(this);
    }
}


//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::GetPointExtent(int ext[6])
{
  ext[0] = this->CellExtent[0];
  ext[1] = this->CellExtent[1]+1;
  ext[2] = this->CellExtent[2];
  ext[3] = this->CellExtent[3]+1;
  ext[4] = this->CellExtent[4];
  ext[5] = this->CellExtent[5]+1;
}
//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::GetCellExtent(int ext[6])
{
  memcpy(ext, this->CellExtent, 6*sizeof(int));
}
//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::GetCellIncrements(int incs[3])
{
  incs[0] = this->CellIncrements[0];
  incs[1] = this->CellIncrements[1];
  incs[2] = this->CellIncrements[2];
}
//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::GetBaseCellExtent(int ext[6])
{
  // Since this is taking a significant amount of time in the
  // GetNeighborIterator method, wemight consider storing
  // the base cell extent directly.
  *ext++ = this->BaseCellExtent[0];
  *ext++ = this->BaseCellExtent[1];
  *ext++ = this->BaseCellExtent[2];
  *ext++ = this->BaseCellExtent[3];
  *ext++ = this->BaseCellExtent[4];
  *ext++ = this->BaseCellExtent[5];
}

//----------------------------------------------------------------------------
//Returns a pointer to the minimum AMR Extent with is the first
// cell values that is not a ghost cell.
unsigned char* vtkCTHFragmentConnectBlock::GetBaseVolumeFractionPointer()
{
  unsigned char* ptr = this->VolumeFractionArray;

  ptr += this->CellIncrements[0] * (this->BaseCellExtent[0] 
                                    - this->CellExtent[0]);
  ptr += this->CellIncrements[1] * (this->BaseCellExtent[2] 
                                    - this->CellExtent[2]);
  ptr += this->CellIncrements[2] * (this->BaseCellExtent[4] 
                                    - this->CellExtent[4]);
  
  return ptr;
}

//----------------------------------------------------------------------------
int* vtkCTHFragmentConnectBlock::GetBaseFragmentIdPointer()
{
  int* ptr = this->FragmentIds;

  ptr += this->CellIncrements[0] * (this->BaseCellExtent[0] 
                                    - this->CellExtent[0]);
  ptr += this->CellIncrements[1] * (this->BaseCellExtent[2] 
                                    - this->CellExtent[2]);
  ptr += this->CellIncrements[2] * (this->BaseCellExtent[4] 
                                    - this->CellExtent[4]);

  return ptr;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnectBlock::ExtractExtent(unsigned char* buf, int ext[6])
{
  // Initialize the buffer to 0.
  memset(buf,0,(ext[1]-ext[0]+1)*(ext[3]-ext[2]+1)*(ext[5]-ext[4]+1));

  unsigned char* volumeFractionPointer = this->VolumeFractionArray;

  // Compute increments.
  int inc0 = this->CellIncrements[0];
  int inc1 = this->CellIncrements[1];
  int inc2 = this->CellIncrements[2];
  int cellExtent[6];
  this->GetCellExtent(cellExtent);
  unsigned char* ptr0;
  unsigned char* ptr1;
  unsigned char* ptr2;
  // Find the starting pointer.
  ptr2 = volumeFractionPointer + inc0*(ext[0]-cellExtent[0])
                               + inc1*(ext[2]-cellExtent[2])
                               + inc2*(ext[4]-cellExtent[4]);
  int xx, yy, zz;
  for (zz = ext[4]; zz <= ext[5]; ++zz)
    {
    ptr1 = ptr2;
    for (yy = ext[2]; yy <= ext[3]; ++yy)
      {
      ptr0 = ptr1;
      for (xx = ext[0]; xx <= ext[1]; ++xx)
        {
        *buf = *ptr0;
        ++buf;
        ptr0 += inc0;
        }
      ptr1 += inc1;
      }
    ptr2 += inc2;
    }
}


//============================================================================
// A class to make finding neighbor blocks faster.
// We assume that each level has a regular array of blocks.
// Of course blocks may be missing from the array.
class vtkCTHFragmentLevel
{
public:
  vtkCTHFragmentLevel();
  ~vtkCTHFragmentLevel();
  
  // Description:
  // Block dimensions should be cell dimensions without overlap.
  // GridExtent is the extent of block indexes. (Block 1, block 2, ...)
  void Initialize(int gridExtent[6], int Level);
  
  // Description:
  // Called after initialization.
  void SetStandardBlockDimensions(int dims[3]);
  
  // Description:
  // This adds a block.  Return VTK_ERROR if the block is not in the
  // correct extent (VTK_OK otherwise).
  int AddBlock(vtkCTHFragmentConnectBlock* block);
  
  // Description:
  // Get the block at this grid index.
  vtkCTHFragmentConnectBlock* GetBlock(int xIdx, int yIdx, int zIdx);
  
  // Decription:
  // Get the cell dimensions of the blocks (base extent) stored in this level.
  // We assume that all blocks are the same size.
  void GetBlockDimensions(int dims[3]);
  
  // Description:
  // Get the extrent of the grid of blocks for this level.
  // Extent should be in cell coordinates.
  void GetGridExtent(int ext[6]);

private:

  int Level;
  int GridExtent[6];
  int BlockDimensions[3];
  
  vtkCTHFragmentConnectBlock** Grid;
};

//----------------------------------------------------------------------------
vtkCTHFragmentLevel::vtkCTHFragmentLevel ()
{
  this->Level = 0;
  for (int ii = 0; ii < 6; ++ii)
    {
    this->GridExtent[ii] = 0;
    }
  this->BlockDimensions[0] = 0;
  this->BlockDimensions[1] = 0;
  this->BlockDimensions[2] = 0;
  this->Grid = 0;
}
//----------------------------------------------------------------------------
vtkCTHFragmentLevel::~vtkCTHFragmentLevel()
{
  this->Level = 0;

  this->BlockDimensions[0] = 0;
  this->BlockDimensions[1] = 0;
  this->BlockDimensions[2] = 0;
  if (this->Grid)
    {
    int num = (this->GridExtent[1]-this->GridExtent[0]+1)
                * (this->GridExtent[3]-this->GridExtent[2]+1)
                * (this->GridExtent[5]-this->GridExtent[4]+1);
    for (int ii = 0; ii < num; ++ii)
      {
      if (this->Grid[ii])
        {
        // We are not the only container for blocks yet.
        //delete this->Grid[ii];
        this->Grid[ii] = 0;
        }
      }
    delete [] this->Grid;
    }
  for (int ii = 0; ii < 6; ++ii)
    {
    this->GridExtent[ii] = 0;
    }
}

//----------------------------------------------------------------------------
void vtkCTHFragmentLevel::GetBlockDimensions(int dims[3])
{
  dims[0] = this->BlockDimensions[0];
  dims[1] = this->BlockDimensions[1];
  dims[2] = this->BlockDimensions[2];
}

//----------------------------------------------------------------------------
void vtkCTHFragmentLevel::GetGridExtent(int ext[6])
{
  for (int ii = 0; ii < 6; ++ii)
    {
    ext[ii] = this->GridExtent[ii];
    }
}

//----------------------------------------------------------------------------
void vtkCTHFragmentLevel::Initialize(
  int gridExtent[6],
  int level)
{
  if (this->Grid)
    {
    vtkGenericWarningMacro("Level already initialized.");
    return;
    }
  // Special case for a level with no blocks in it.
  if (gridExtent[0] > gridExtent[1] || gridExtent[2] > gridExtent[3] || gridExtent[4] > gridExtent[5])
    { // This should do the trick.
    gridExtent[0] = gridExtent[1] = gridExtent[2] = gridExtent[3] = gridExtent[4] = gridExtent[5] = 0;
    }

  this->Level = level;
  memcpy(this->GridExtent, gridExtent, 6*sizeof(int));
  int num = (this->GridExtent[1]-this->GridExtent[0]+1)
            * (this->GridExtent[3]-this->GridExtent[2]+1)
            * (this->GridExtent[5]-this->GridExtent[4]+1);
  this->Grid = new vtkCTHFragmentConnectBlock*[num];
  memset(this->Grid,0,num*sizeof(vtkCTHFragmentConnectBlock*));
}


//----------------------------------------------------------------------------
void vtkCTHFragmentLevel::SetStandardBlockDimensions(int dims[3])
{
  this->BlockDimensions[0] = dims[0];
  this->BlockDimensions[1] = dims[1];
  this->BlockDimensions[2] = dims[2];
}

//----------------------------------------------------------------------------
int vtkCTHFragmentLevel::AddBlock(vtkCTHFragmentConnectBlock* block)
{
  int xIdx, yIdx, zIdx;
  
  // First make sure the level is correct.
  // We assume that the block dimensions are correct.
  if (block->GetLevel() != this->Level)
    {
    vtkGenericWarningMacro("Wrong level.");
    return VTK_ERROR;
    }
  const int *ext;
  ext = block->GetBaseCellExtent();
  
  if (ext[0] < 0 || ext[2] < 0 || ext[4] < 0)
    {
    vtkGenericWarningMacro("I did not code this for negative extents.");
    }
  
  // Compute the extent from the left most cell value.
  xIdx = ext[0]/this->BlockDimensions[0];
  yIdx = ext[2]/this->BlockDimensions[1];
  zIdx = ext[4]/this->BlockDimensions[2];
  
  if (xIdx < this->GridExtent[0] || xIdx > this->GridExtent[1] ||
      yIdx < this->GridExtent[2] || yIdx > this->GridExtent[3] ||
      zIdx < this->GridExtent[4] || zIdx > this->GridExtent[5])
    {
    vtkGenericWarningMacro("Block index out of grid.");
    return VTK_ERROR;
    }
  
  xIdx -= this->GridExtent[0];
  yIdx -= this->GridExtent[2];
  zIdx -= this->GridExtent[4];
  int idx = xIdx+(yIdx+(zIdx*(this->GridExtent[3]-this->GridExtent[2]+1)))
                             *(this->GridExtent[1]-this->GridExtent[0]+1);

  if (this->Grid[idx])
    {
    vtkGenericWarningMacro("Overwriting block in grid");
    }
  this->Grid[idx] = block;

  return VTK_OK;
}

//----------------------------------------------------------------------------
vtkCTHFragmentConnectBlock* vtkCTHFragmentLevel::GetBlock(
  int xIdx, int yIdx, int zIdx)
{
  if (xIdx < this->GridExtent[0] || xIdx > this->GridExtent[1] ||
      yIdx < this->GridExtent[2] || yIdx > this->GridExtent[3] ||
      zIdx < this->GridExtent[4] || zIdx > this->GridExtent[5])
    {
    return 0;
    }
  xIdx -= this->GridExtent[0];
  yIdx -= this->GridExtent[2];
  zIdx -= this->GridExtent[4];
  int yInc = (this->GridExtent[1]-this->GridExtent[0]+1);
  int zInc = yInc * (this->GridExtent[3]-this->GridExtent[2]+1);
  return this->Grid[xIdx + yIdx*yInc + zIdx*zInc];
}

//============================================================================

//----------------------------------------------------------------------------
// A class to help traverse pointers acrosss block boundaries.
// I am keeping this class to a minimum because many are kept on the stack
// durring recursive connect algorithm.
class vtkCTHFragmentConnectIterator
{
public:
  vtkCTHFragmentConnectIterator() {this->Initialize();}
  ~vtkCTHFragmentConnectIterator() {this->Initialize();}
  void Initialize();

  vtkCTHFragmentConnectBlock* Block;
  unsigned char*              VolumeFractionPointer;
  int*                        FragmentIdPointer;
  int                         Index[3];
};
void vtkCTHFragmentConnectIterator::Initialize()
{
  this->Block = 0;
  this->VolumeFractionPointer = 0;
  this->FragmentIdPointer = 0;
  this->Index[0] = this->Index[1] = this->Index[2] = 0;
}


//============================================================================

//----------------------------------------------------------------------------
// A simple first in last out ring container to hold the seeds for the 
// breadth first search.
// I am going to have the container allocate and delete its own iterators.
// It will simplify the converation from depth first to breadth first.
class vtkCTHFragmentConnectRingBuffer
{
public:
  vtkCTHFragmentConnectRingBuffer();
  ~vtkCTHFragmentConnectRingBuffer();

  void Push(vtkCTHFragmentConnectIterator* iterator);
  int Pop(vtkCTHFragmentConnectIterator* iterator);

  long GetSize() { return this->Size;}


private:

  vtkCTHFragmentConnectIterator* Ring;
  vtkCTHFragmentConnectIterator* End;
  long RingLength;
  // The first and last iterator added.
  vtkCTHFragmentConnectIterator* First;
  vtkCTHFragmentConnectIterator* Next;
  // I could do without this size, but it does not cost much.
  long Size;

  void GrowRing();
};

//----------------------------------------------------------------------------
vtkCTHFragmentConnectRingBuffer::vtkCTHFragmentConnectRingBuffer()
{
  int initialSize = 2000;
  this->Ring = new vtkCTHFragmentConnectIterator[initialSize];
  this->RingLength = initialSize;
  this->End = this->Ring + this->RingLength;
  this->First = 0;
  this->Next = this->Ring;
  this->Size = 0;
}

//----------------------------------------------------------------------------
vtkCTHFragmentConnectRingBuffer::~vtkCTHFragmentConnectRingBuffer()
{
  delete [] this->Ring;
  this->End = 0;
  this->RingLength = 0;
  this->Next = this->First = 0;
  this->Size = 0;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnectRingBuffer::GrowRing()
{
  // Allocate a new ring.
  vtkCTHFragmentConnectIterator* newRing;
  int newRingLength = this->RingLength * 2;
  newRing = new vtkCTHFragmentConnectIterator[newRingLength*2];
  
  // Copy items into the new ring.
  int count = this->Size;
  vtkCTHFragmentConnectIterator* ptr1 = this->First;
  vtkCTHFragmentConnectIterator* ptr2 = newRing;
  while (count > 0)
    {
    *ptr2++ = *ptr1++;
    if (ptr1 == this->End)
      {
      ptr1 = this->Ring;
      }
    --count;
    }

  //cerr << "Grow ring buffer: " << newRingLength << endl;
  
  // Replace the ring.
  // Size remains the same.
  delete [] this->Ring;
  this->Ring = newRing;
  this->End = newRing + newRingLength;
  this->RingLength = newRingLength;
  this->First = newRing;
  this->Next = newRing + this->Size;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnectRingBuffer::Push(vtkCTHFragmentConnectIterator* item)
{
  // Make the ring buffer larger if necessary.
  if (this->Size == this->RingLength)
    {
    this->GrowRing();
    }
  
  // Add the item.
  *(this->Next) = *item;
  // Special case for an empty ring.
  // We could initialize start to next to avoid this condition every push.
  if (this->Size == 0)
    {
    this->First = this->Next;
    }

  // Move the the next-available-slot pointer to the next space.
  ++this->Next;
  // End of the linear buffer moves back to the begining.
  // This makes it a ring.
  if (this->Next == this->End)
    {
    this->Next = this->Ring;
    }

  ++this->Size;
}

//----------------------------------------------------------------------------
int vtkCTHFragmentConnectRingBuffer::Pop(vtkCTHFragmentConnectIterator* item)
{
  if (this->Size == 0)
    {
    return 0;
    }
    
  *item = *(this->First);
  //this->First->Initialize();
  ++this->First;
  --this->Size;

  // End of the linear buffer moves back to the begining.
  // This makes it a ring.
  if (this->First == this->End)
    {
    this->First = this->Ring;
    }
    
  return 1;
}


//============================================================================



//----------------------------------------------------------------------------
// Description:
// Construct object with initial range (0,1) and single contour value
// of 0.0. ComputeNormal is on, ComputeGradients is off and ComputeScalars is on.
vtkCTHFragmentConnect::vtkCTHFragmentConnect()
{
  this->VolumeFractionArrayName = 0;

  this->NumberOfInputBlocks = 0;
  this->InputBlocks = 0;
  this->Controller = vtkMultiProcessController::GetGlobalController();
  this->GlobalOrigin[0]=this->GlobalOrigin[1]=this->GlobalOrigin[2]=0.0;
  this->RootSpacing[0]=this->RootSpacing[1]=this->RootSpacing[2]=1.0;

  this->FragmentId = 0;
  this->FragmentVolume = 0.0;
  this->FragmentVolumes = vtkDoubleArray::New();
  this->EquivalenceSet = new vtkCTHFragmentEquivalenceSet;
  this->LocalToGlobalOffsets = 0;
  this->TotalNumberOfRawFragments = 0;
  this->NumberOfResolvedFragments = 0;
  this->IntegratedFragmentAttributes = 0;
  this->IntegrationBuffer = 0;
  
  this->FaceNeighbors =  new vtkCTHFragmentConnectIterator[32];
}

//----------------------------------------------------------------------------
vtkCTHFragmentConnect::~vtkCTHFragmentConnect()
{
  this->DeleteAllBlocks();
  this->Controller = 0;
  this->GlobalOrigin[0]=this->GlobalOrigin[1]=this->GlobalOrigin[2]=0.0;
  this->RootSpacing[0]=this->RootSpacing[1]=this->RootSpacing[2]=1.0;
  
  this->FragmentId = 0;
  this->FragmentVolume = 0.0;
  this->FragmentVolumes->Delete();
  this->FragmentVolumes = 0;

  delete this->EquivalenceSet;
  this->EquivalenceSet = 0;
  this->IntegratedFragmentAttributes = 0;
  this->IntegrationBuffer = 0;
  
  delete this->FaceNeighbors;
  this->FaceNeighbors = 0;
}


//----------------------------------------------------------------------------
// Called to delete all the block structures.
void vtkCTHFragmentConnect::DeleteAllBlocks()
{
  if (this->NumberOfInputBlocks == 0)
    {
    return;
    }
  
  // Ghost blocks
  int num = this->GhostBlocks.size();
  int ii;
  vtkCTHFragmentConnectBlock* block;
  for (ii = 0; ii < num; ++ii)
    {
    block = this->GhostBlocks[ii];
    delete block;
    }
  this->GhostBlocks.clear();

  // Normal Blocks
  for (ii = 0; ii < this->NumberOfInputBlocks; ++ii)
    {
    if (this->InputBlocks[ii])
      {
      delete this->InputBlocks[ii];
      this->InputBlocks[ii] = 0;
      }
    }
  if (this->InputBlocks)
    {
    delete [] this->InputBlocks;
    this->InputBlocks = 0;
    }
  this->NumberOfInputBlocks = 0;
  
  // levels
  int level, numLevels;
  numLevels = this->Levels.size();
  for (level = 0; level < numLevels; ++level)
    {
    if (this->Levels[level])
      {
      delete this->Levels[level];
      this->Levels[level] = 0;
      }
    }
}



//----------------------------------------------------------------------------
// Initialize a single block from an image input.
int vtkCTHFragmentConnect::InitializeBlocks(vtkImageData* input)
{
  // Just in case
  this->DeleteAllBlocks();

  // TODO: We need to check the CELL_DATA and the correct volume fraction array.

  vtkCTHFragmentConnectBlock* block;
  this->InputBlocks = new vtkCTHFragmentConnectBlock*[1];
  this->NumberOfInputBlocks = 1;
  block = this->InputBlocks[0] = new vtkCTHFragmentConnectBlock;
  block->Initialize(0, input, 0, input->GetOrigin(), input->GetSpacing(),
                    this->VolumeFractionArrayName);

  return VTK_OK;
}

//----------------------------------------------------------------------------
// Initialize blocks from multi block input.
int vtkCTHFragmentConnect::InitializeBlocks(vtkHierarchicalBoxDataSet* input)
{
  int level;
  int numLevels = input->GetNumberOfLevels();
  vtkCTHFragmentConnectBlock* block;
  int myProc = this->Controller->GetLocalProcessId();
  int numProcs = this->Controller->GetNumberOfProcesses();

  // Just in case
  this->DeleteAllBlocks();

  //cerr << "start processing blocks\n" << endl;

  // We need all blocks to share a common extent index.
  // We find a common origin as the minimum most point in level 0.
  // Since invalid ghost cells are removed, this should be correct.
  // However, it should not matter because any point on root grid that
  // keeps all indexes positive will do.
  this->NumberOfInputBlocks = this->ComputeOriginAndRootSpacing(input);

  // Create the array of blocks.
  this->InputBlocks = new vtkCTHFragmentConnectBlock*[this->NumberOfInputBlocks];
  // Initilize to NULL.
  for (int blockId = 0; blockId < this->NumberOfInputBlocks; ++blockId)
    {
    this->InputBlocks[blockId] = 0;
    }
 
  // Initialize each block with the input image 
  // and global index coordinate system.
  int blockIndex = -1;
  this->Levels.resize(numLevels);
  for (level = 0; level < numLevels; ++level)
    {
    this->Levels[level] = new vtkCTHFragmentLevel;

    int cumulativeExt[6];
    cumulativeExt[0] = cumulativeExt[2] = cumulativeExt[4] = VTK_LARGE_INTEGER;
    cumulativeExt[1] = cumulativeExt[3] = cumulativeExt[5] = -VTK_LARGE_INTEGER;
    
    int numBlocks = input->GetNumberOfDataSets(level);
    for (int levelBlockId = 0; levelBlockId < numBlocks; ++levelBlockId)
      {
      vtkAMRBox box;
      vtkImageData* image = input->GetDataSet(level,levelBlockId,box);
      // TODO: We need to check the CELL_DATA and the correct volume fraction array.

      if (image)
        {
        block = this->InputBlocks[++blockIndex] = new vtkCTHFragmentConnectBlock;
        // Do we really need the block to know its id? 
        // We use it to find neighbors.  We should save pointers directly in neighbor array.
        // We also use it for debugging.
        block->Initialize(blockIndex, image, level, this->GlobalOrigin, this->RootSpacing,
                          this->VolumeFractionArrayName);
        // For debugging:
        block->LevelBlockId = levelBlockId;
        
        // Collect information about the blocks in this level.
        const int *ext;
        ext = block->GetBaseCellExtent();
        // We need the cumulative extent to determine the grid extent.
        if (cumulativeExt[0] > ext[0]) {cumulativeExt[0] = ext[0];}
        if (cumulativeExt[1] < ext[1]) {cumulativeExt[1] = ext[1];}
        if (cumulativeExt[2] > ext[2]) {cumulativeExt[2] = ext[2];}
        if (cumulativeExt[3] < ext[3]) {cumulativeExt[3] = ext[3];}
        if (cumulativeExt[4] > ext[4]) {cumulativeExt[4] = ext[4];}
        if (cumulativeExt[5] < ext[5]) {cumulativeExt[5] = ext[5];}
        }
      }

    // Expand the grid extent by 1 in all directions to accomadate ghost blocks.
    // We might have a problem with level 0 here since blockDims is not global yet.
    cumulativeExt[0] = cumulativeExt[0] / this->StandardBlockDimensions[0];
    cumulativeExt[1] = cumulativeExt[1] / this->StandardBlockDimensions[0];
    cumulativeExt[2] = cumulativeExt[2] / this->StandardBlockDimensions[0];
    cumulativeExt[3] = cumulativeExt[3] / this->StandardBlockDimensions[0];
    cumulativeExt[4] = cumulativeExt[4] / this->StandardBlockDimensions[0];
    cumulativeExt[5] = cumulativeExt[5] / this->StandardBlockDimensions[0];

    // Expand extent to cover all processes.
    if (myProc > 0)
      {
      this->Controller->Send(cumulativeExt, 6, 0, 212130);
      this->Controller->Receive(cumulativeExt, 6, 0, 212131);
      }
    else
      {
      int tmp[6];
      for (int ii = 1; ii < numProcs; ++ii)
        {
        this->Controller->Receive(tmp, 6, ii, 212130);
        if (cumulativeExt[0] > tmp[0]) {cumulativeExt[0] = tmp[0];}
        if (cumulativeExt[1] < tmp[1]) {cumulativeExt[1] = tmp[1];}
        if (cumulativeExt[2] > tmp[2]) {cumulativeExt[2] = tmp[2];}
        if (cumulativeExt[3] < tmp[3]) {cumulativeExt[3] = tmp[3];}
        if (cumulativeExt[4] > tmp[4]) {cumulativeExt[4] = tmp[4];}
        if (cumulativeExt[5] < tmp[5]) {cumulativeExt[5] = tmp[5];}
        }
      // Redistribute the global grid extent
      for (int ii = 1; ii < numProcs; ++ii)
        {
        this->Controller->Send(cumulativeExt, 6, ii, 212131);
        }
      }

    this->Levels[level]->Initialize(cumulativeExt, level);
    this->Levels[level]->SetStandardBlockDimensions(this->StandardBlockDimensions);
    }

  // Now add all the blocks to the level structures.
  int ii;
  for (ii = 0; ii < this->NumberOfInputBlocks; ++ii)
    {
    block = this->InputBlocks[ii];
    this->AddBlock(block);
    }

  //cerr << "start ghost blocks\n" << endl;

  // Broadcast all of the block meta data to all processes.
  // Setup ghost layer blocks.
  // Remove this until the local version is working again.....
  if (this->Controller && this->Controller->GetNumberOfProcesses() > 1)
    {
    this->ShareGhostBlocks();
    }
    
  return VTK_OK;
}



//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::CheckLevelsForNeighbors(
  vtkCTHFragmentConnectBlock* block)
{
  vtkstd::vector<vtkCTHFragmentConnectBlock*> neighbors;
  vtkCTHFragmentConnectBlock* neighbor;
  int blockIndex[3];

  // Extract the index from the block extent.
  const int *ext;
  ext = block->GetBaseCellExtent();
  blockIndex[0] = ext[0] / this->StandardBlockDimensions[0];
  blockIndex[1] = ext[2] / this->StandardBlockDimensions[1];
  blockIndex[2] = ext[4] / this->StandardBlockDimensions[2];

  for (int axis = 0; axis < 3; ++axis)
    {
    // The purpose for passing a list into the method is no longer.
    // We could simplify this method.
    neighbors.clear();
    this->FindFaceNeighbors(block->GetLevel(), blockIndex, 
                            axis, 0, &neighbors);
    for (unsigned int ii = 0; ii < neighbors.size(); ++ii)
      {
      neighbor = neighbors[ii];
      block->AddNeighbor(neighbor, axis, 0);
      neighbor->AddNeighbor(block, axis, 1);
      }

    this->FindFaceNeighbors(block->GetLevel(), blockIndex,
                           axis, 1, &neighbors);
    for (unsigned int ii = 0; ii < neighbors.size(); ++ii)
      {
      neighbor = neighbors[ii];
      block->AddNeighbor(neighbor, axis, 1);
      neighbor->AddNeighbor(block, axis, 0);
      }
    }
}



//----------------------------------------------------------------------------
// trying to make a single method to find neioghbors that can be used
// to both add a new block and calculate what ghost extent we need.
// Returns 1 if at least one non-ghost neighbor exists.
int vtkCTHFragmentConnect::FindFaceNeighbors(
  unsigned int blockLevel,
  int blockIndex[3],
  int faceAxis,
  int faceMaxFlag,
  vtkstd::vector<vtkCTHFragmentConnectBlock*> *result)
{
  int retVal = 0;
  vtkCTHFragmentConnectBlock* neighbor;
  int idx[3];
  int tmp[3];
  int levelDifference;
  int p2;
  int axis1 = (faceAxis+1)%3;
  int axis2 = (faceAxis+2)%3;


  for (unsigned int level = 0; level < this->Levels.size(); ++level)
    {
    // We convert between cell index and point index to do level conversion.
    idx[faceAxis] = blockIndex[faceAxis] + faceMaxFlag;
    idx[axis1] = blockIndex[axis1];
    idx[axis2] = blockIndex[axis2];
    
    if (level <= blockLevel)
      { // Neighbor block is larger than reference block.
      levelDifference = blockLevel - level;
      // If the point location is divisible by the power of two factor then the
      // face lies on the level grids boundary face and we might have a neighbor.
      if ((idx[faceAxis] >> levelDifference) << levelDifference == idx[faceAxis])
        { // Face lies on a boundary
        // Convert index to working level and back to cell index.
        tmp[0] = idx[0] >> levelDifference;
        tmp[1] = idx[1] >> levelDifference;
        tmp[2] = idx[2] >> levelDifference;
        if ( ! faceMaxFlag)
          { // min face.  Neighbor one to "left".
          tmp[faceAxis] -= 1;
          }
        neighbor = this->Levels[level]->GetBlock(tmp[0], tmp[1], tmp[2]);
        if (neighbor)
          {
          // Useful for compute required ghost extents.
          if ( ! neighbor->GetGhostFlag())
            {
            retVal = 1;
            }
          result->push_back(neighbor);
          }
        }
      }
    else
      { // Neighbor block is smaller than reference block.
      // All faces will be on boundary.
      // Convert index to working level.
      levelDifference =  level - blockLevel;
      p2 = 1 << levelDifference;
      // Point index of face for easier level conversion.
      idx[0] = idx[0] << levelDifference;
      idx[1] = idx[1] << levelDifference;
      idx[2] = idx[2] << levelDifference;
      if ( ! faceMaxFlag)
        { // min face.  Neighbor one to "left".
        idx[faceAxis] -= 1;
        }
      // Loop over all possible blocks touching this face.
      tmp[faceAxis] = idx[faceAxis];
      for (int ii = 0; ii < p2; ++ii)
        {
        tmp[axis1] = idx[axis1] + ii;
        for (int jj = 0; jj < p2; ++jj)
          {
          tmp[axis2] = idx[axis2] + jj;
          neighbor = this->Levels[level]->GetBlock(tmp[0], tmp[1], tmp[2]);
          if (neighbor)
            {
            // Useful for compute required ghost extents.
            if ( ! neighbor->GetGhostFlag())
              {
              retVal = 1;
              }
            result->push_back(neighbor);
            }
          }
        }
      }
    }
  return retVal;
}


//----------------------------------------------------------------------------
// We need ghost cells for edges and corners as well as faces.
// neighborDirection is used to specify a face, edge or corner.
// Using a 2x2x2 cube center at origin: (-1,-1,-1), (-1,-1,1) ... are corners.
// (1,1,0) is an edge, and (-1,0,0) is a face.
// Returns 1 if the neighbor exists.
int vtkCTHFragmentConnect::HasNeighbor(
  unsigned int blockLevel,
  int blockIndex[3],
  int neighborDirection[3])
{
  vtkCTHFragmentConnectBlock* neighbor;
  int idx[3];
  int levelDifference;

  // Check all levels.
  for (unsigned int level = 0; level < this->Levels.size(); ++level)
    {
    // We convert between cell index and point index to do level conversion.
    if (level <= blockLevel)
      { // Neighbor block is larger than reference block.
      levelDifference = blockLevel - level;
      // Check to see that all non zero axes lie on a grid boundary.
      int bdFlag = 1; // Is the face, edge or corner on a grid boundary?
      for (int ii = 0; ii < 3; ++ii)
        {
        switch (neighborDirection[ii])
          {
          case -1:
            idx[ii] = (blockIndex[ii] >> levelDifference) - 1;
            if ((blockIndex[ii] >> levelDifference) << levelDifference != blockIndex[ii])
              {
              bdFlag = 0;
              }
            break;
          case 1:
            idx[ii] = (blockIndex[ii] >> levelDifference) + 1;
            if (idx[ii] << levelDifference != blockIndex[ii] + 1)
              {
              bdFlag = 0;
              }
            break;
          case 0:
            idx[ii] = (blockIndex[ii] >> levelDifference);
          }
        }
      if (bdFlag)
        { // Neighbor primative direction is on grid boundary.
        neighbor = this->Levels[level]->GetBlock(idx[0], idx[1], idx[2]);
        if (neighbor && ! neighbor->GetGhostFlag())
          { // We found one.  Thats all we need.
          return 1;
          }
        }
      }
    else
      { // Neighbor block is smaller than reference block.
      // All faces will be on boundary.
      // !!! We have to loop over all axes whose direction component is 0.
      // Convert index to working level.
      levelDifference =  level - blockLevel;
      int mins[3];
      int maxs[3];
      int ix, iy, iz;

      // Compupte the range of potential neighbor indexes.
      for (int ii = 0; ii < 3; ++ii)
        {
        switch (neighborDirection[ii])
          {
          case -1:
            mins[ii] = maxs[ii] = (blockIndex[ii] << levelDifference) - 1;
            break;
          case 0:
            mins[ii] = (blockIndex[ii] << levelDifference);
            maxs[ii] = mins[ii] + (1 << levelDifference) - 1;
            break;
          case 1:
            mins[ii] = maxs[ii] = (blockIndex[ii]+1) << levelDifference;
          }
        }
      // Now do the loops.
      for (ix = mins[0]; ix <= maxs[0]; ++ix)
        {
        for (iy = mins[1]; iy <= maxs[1]; ++iy)
          {
          for (iz = mins[2]; iz <= maxs[2]; ++iz)
            {
            neighbor = this->Levels[level]->GetBlock(ix, iy, iz);
            if (neighbor && ! neighbor->GetGhostFlag())
              { // We found one.  Thats all we need.
              return 1;
              }
            }
          }
        }
      }
    }
  return 0;
}



//----------------------------------------------------------------------------
// All processes must share a common origin.
// Returns the total number of blocks in all levels (this process only).
// This computes:  GlobalOrigin, RootSpacing and StandardBlockDimensions.
// StandardBlockDimensions are the size of blocks without the extra
// overlap layer put on by spyplot format.
// RootSpacing is the spacing that blocks in level 0 would have.
// GlobalOrigin is choosen so that there are no negative extents and
// base extents (without overlap/ghost buffer) lie on grid 
// (i.e.) the min base extent must be a multiple of the standardBlockDimesions.
int vtkCTHFragmentConnect::ComputeOriginAndRootSpacing(
  vtkHierarchicalBoxDataSet* input)
{
  int numLevels = input->GetNumberOfLevels();
  int numBlocks;
  int blockId;
  int totalNumberOfBlocksInThisProcess = 0;
  
  // This is a big pain.
  // We have to look through all blocks to get a minimum root origin.
  // The origin must be choosen so there are no negative indexes.
  // Negative indexes would require the use floor or ceiling function instead
  // of simple truncation.
  //  The origin must also lie on the root grid.
  // The big pain is finding the correct origin when we do not know which
  // blocks have ghost layers.  The Spyplot reader strips
  // ghost layers from outside blocks.

  // Overall processes:
  // Find the largest of all block dimensions to compute standard dimensions.
  // Save the largest block information.
  // Find the overall bounds of the data set.
  // Find one of the lowest level blocks to compute origin.

  int    lowestLevel;
  double lowestSpacing[3];
  double lowestOrigin[3];
  int    lowestDims[3];
  int    largestLevel;
  double largestOrigin[3];
  double largestSpacing[3];
  int    largestDims[3];
  int    largestNumCells;
  
  double globalBounds[6];

  // Temporary variables.
  double spacing[3];
  double bounds[6];
  int cellDims[3];
  int numCells;
  int ext[6];

  largestNumCells = 0;
  globalBounds[0] = globalBounds[2] = globalBounds[4] = VTK_LARGE_FLOAT;
  globalBounds[1] = globalBounds[3] = globalBounds[5] = -VTK_LARGE_FLOAT;
  lowestSpacing[0] = lowestSpacing[1] = lowestSpacing[2] = 0.0;

  // Add each block.
  for (int level = 0; level < numLevels; ++level)
    {
    numBlocks = input->GetNumberOfDataSets(level);
    for (blockId = 0; blockId < numBlocks; ++blockId)
      {
      vtkAMRBox box;
      vtkImageData* image = input->GetDataSet(level,blockId,box);
      if (image)
        {
        ++totalNumberOfBlocksInThisProcess;
        image->GetBounds(bounds);
        // Compute globalBounds.
        if (globalBounds[0] > bounds[0]) {globalBounds[0] = bounds[0];}
        if (globalBounds[1] < bounds[1]) {globalBounds[1] = bounds[1];}
        if (globalBounds[2] > bounds[2]) {globalBounds[2] = bounds[2];}
        if (globalBounds[3] < bounds[3]) {globalBounds[3] = bounds[3];}
        if (globalBounds[4] > bounds[4]) {globalBounds[4] = bounds[4];}
        if (globalBounds[5] < bounds[5]) {globalBounds[5] = bounds[5];}
        image->GetExtent(ext);
        cellDims[0] = ext[1]-ext[0]; // ext is point extent.
        cellDims[1] = ext[3]-ext[2];
        cellDims[2] = ext[5]-ext[4];
        numCells = cellDims[0] * cellDims[1] * cellDims[2];
        // Compute standard block dimensions.
        if (numCells > largestNumCells)
          {
          largestDims[0] = cellDims[0];
          largestDims[1] = cellDims[1];
          largestDims[2] = cellDims[2];
          largestNumCells = numCells;
          image->GetOrigin(largestOrigin);
          image->GetSpacing(largestSpacing);
          largestLevel = level;
          }
        // Find the lowest level block.
        image->GetSpacing(spacing);
        if (spacing[0] > lowestSpacing[0]) // Only test axis 0. Assume others agree.
          { // This is the lowest level block we have encountered.
          image->GetSpacing(lowestSpacing);
          lowestLevel = level;
          image->GetOrigin(lowestOrigin);
          lowestDims[0] = cellDims[0];
          lowestDims[1] = cellDims[1];
          lowestDims[2] = cellDims[2];
          }
        }
      }
    }

  // Send the results to process 0 that will choose the origin ...
  double dMsg[18];
  int    iMsg[9];
  int myId = 0;
  int numProcs = 1;
  vtkMultiProcessController* controller = vtkMultiProcessController::GetGlobalController();
  if (controller)
    {
    numProcs = controller->GetNumberOfProcesses();
    myId = controller->GetLocalProcessId();
    if (myId > 0)
      { // Send to process 0.
      iMsg[0] = lowestLevel;
      iMsg[1] = largestLevel;
      iMsg[2] = largestNumCells;
      for (int ii= 0; ii < 3; ++ii)
        {
        iMsg[3+ii] = lowestDims[ii];
        iMsg[6+ii] = largestDims[ii];
        dMsg[ii]   = lowestSpacing[ii];
        dMsg[3+ii] = lowestOrigin[ii];
        dMsg[6+ii] = largestOrigin[ii];
        dMsg[9+ii] = largestSpacing[ii];
        dMsg[12+ii] = globalBounds[ii];
        dMsg[15+ii] = globalBounds[ii+3];
        }
      controller->Send(iMsg, 9, 0, 8973432);
      controller->Send(dMsg, 15, 0, 8973432);
      }
    else
      {
      // Collect results from all processes.
      for (int id = 1; id < numProcs; ++id)
        {
        controller->Receive(iMsg, 9, id, 8973432);
        controller->Receive(dMsg, 18, id, 8973432);
        numCells = iMsg[2];
        cellDims[0] = iMsg[6];
        cellDims[1] = iMsg[7];
        cellDims[2] = iMsg[8];
        if (numCells > largestNumCells)
          {
          largestDims[0] = cellDims[0];
          largestDims[1] = cellDims[1];
          largestDims[2] = cellDims[2];
          largestNumCells = numCells;
          largestOrigin[0] = dMsg[6];
          largestOrigin[1] = dMsg[7];
          largestOrigin[2] = dMsg[8];
          largestSpacing[0] = dMsg[9];
          largestSpacing[1] = dMsg[10];
          largestSpacing[2] = dMsg[11];
          largestLevel = iMsg[1];
          }
        // Find the lowest level block.
        spacing[0] = dMsg[0];
        spacing[1] = dMsg[1];
        spacing[2] = dMsg[2];
        if (spacing[0] > lowestSpacing[0]) // Only test axis 0. Assume others agree.
          { // This is the lowest level block we have encountered.
          lowestSpacing[0] = spacing[0];
          lowestSpacing[1] = spacing[1];
          lowestSpacing[2] = spacing[2];
          lowestLevel = iMsg[0];
          lowestOrigin[0] = dMsg[3];
          lowestOrigin[1] = dMsg[4];
          lowestOrigin[2] = dMsg[5];
          lowestDims[0] = iMsg[6];
          lowestDims[1] = iMsg[7];
          lowestDims[2] = iMsg[8];
          }
        if (globalBounds[0] > dMsg[9])  {globalBounds[0] = dMsg[9];}
        if (globalBounds[1] < dMsg[10]) {globalBounds[1] = dMsg[10];}
        if (globalBounds[2] > dMsg[11]) {globalBounds[2] = dMsg[11];}
        if (globalBounds[3] < dMsg[12]) {globalBounds[3] = dMsg[12];}
        if (globalBounds[4] > dMsg[13]) {globalBounds[4] = dMsg[13];}
        if (globalBounds[5] < dMsg[14]) {globalBounds[5] = dMsg[14];}
        }
      }
    }

  if (myId == 0)
    {
    this->StandardBlockDimensions[0] = largestDims[0]-2;
    this->StandardBlockDimensions[1] = largestDims[1]-2;
    this->StandardBlockDimensions[2] = largestDims[2]-2;
    this->RootSpacing[0] = lowestSpacing[0] * (1 << (lowestLevel));
    this->RootSpacing[1] = lowestSpacing[1] * (1 << (lowestLevel));
    this->RootSpacing[2] = lowestSpacing[2] * (1 << (lowestLevel));
    // Find the grid for the largest block.  We assume this block has the
    // extra ghost layers.
    largestOrigin[0] = largestOrigin[0] + largestSpacing[0];
    largestOrigin[1] = largestOrigin[1] + largestSpacing[1];
    largestOrigin[2] = largestOrigin[2] + largestSpacing[2];
    // Convert to the spacing of the blocks.
    largestSpacing[0] *= this->StandardBlockDimensions[0];
    largestSpacing[1] *= this->StandardBlockDimensions[1];
    largestSpacing[2] *= this->StandardBlockDimensions[2];
    // Find the point on the grid closest to the lowest level origin.
    // We do not know if this lowest level block has its ghost layers.
    // Even if the dims are one less that standard, which side is missing
    // the ghost layer!
    int idx[3];
    idx[0] = (int)(floor(0.5 + (lowestOrigin[0]-largestOrigin[0]) / largestSpacing[0]));
    idx[1] = (int)(floor(0.5 + (lowestOrigin[1]-largestOrigin[1]) / largestSpacing[1]));
    idx[2] = (int)(floor(0.5 + (lowestOrigin[2]-largestOrigin[2]) / largestSpacing[2]));
    lowestOrigin[0] = largestOrigin[0] + (double)(idx[0])*largestSpacing[0];
    lowestOrigin[1] = largestOrigin[1] + (double)(idx[1])*largestSpacing[1];
    lowestOrigin[2] = largestOrigin[2] + (double)(idx[2])*largestSpacing[2];
    // OK.  Now we have the grid for the lowest level that has a block.
    // Change the grid to be of the blocks.
    lowestSpacing[0] *= this->StandardBlockDimensions[0];
    lowestSpacing[1] *= this->StandardBlockDimensions[1];
    lowestSpacing[2] *= this->StandardBlockDimensions[2];
    
    // Change the origin so that all indexes will be positive.
    idx[0] = (int)(floor((globalBounds[0]-lowestOrigin[0]) / lowestSpacing[0]));
    idx[1] = (int)(floor((globalBounds[2]-lowestOrigin[1]) / lowestSpacing[1]));
    idx[2] = (int)(floor((globalBounds[4]-lowestOrigin[2]) / lowestSpacing[2]));
    this->GlobalOrigin[0] = lowestOrigin[0] + (double)(idx[0])*lowestSpacing[0];
    this->GlobalOrigin[1] = lowestOrigin[1] + (double)(idx[1])*lowestSpacing[1];
    this->GlobalOrigin[2] = lowestOrigin[2] + (double)(idx[2])*lowestSpacing[2];
    
    // Now send these to all the other processes and we are done!
    for (int ii = 0; ii < 3; ++ii)
      {
      dMsg[ii] = this->GlobalOrigin[ii];
      dMsg[ii+3] = this->RootSpacing[ii];
      dMsg[ii+6] = (double)(this->StandardBlockDimensions[ii]);
      }
    for (int ii = 1; ii < numProcs; ++ii)
      {
      controller->Send(dMsg, 9, ii, 8973439);
      }
    }
  else
    {
    controller->Receive(dMsg, 9, 0, 8973439);
    for (int ii = 0; ii < 3; ++ii)
      {
      this->GlobalOrigin[ii] = dMsg[ii];
      this->RootSpacing[ii] = dMsg[ii+3];
      this->StandardBlockDimensions[ii] = (int)(dMsg[ii+6]);
      }
    }
    
  return totalNumberOfBlocksInThisProcess;
}



//----------------------------------------------------------------------------
// This method creates ghost layer blocks across multiple processes.
void vtkCTHFragmentConnect::ShareGhostBlocks()
{
  int numProcs = this->Controller->GetNumberOfProcesses();
  int myProc = this->Controller->GetLocalProcessId();
  vtkCommunicator* com  = this->Controller->GetCommunicator();

  // Bad things can happen if not all processes call 
  // MPI_Alltoallv this at the same time. (mpich)
  this->Controller->Barrier();
  
  // First share the number of blocks.
  int *blocksPerProcess = new int[numProcs];
  com->AllGather(&(this->NumberOfInputBlocks), blocksPerProcess, 1);
  //MPI_Allgather(&(this->NumberOfInputBlocks), 1, MPI_INT,
  //blocksPerProcess, 1, MPI_INT,
  //*com->GetMPIComm()->GetHandle());
  
  // Share the levels and extents of all blocks.
  // First, setup the count and displacement arrays required by AllGatherV.
  int totalNumberOfBlocks = 0;
  int *sendCounts = new int[numProcs];
  vtkIdType *recvCounts = new vtkIdType[numProcs];
  vtkIdType *displacements = new vtkIdType[numProcs];
  for (int ii = 0; ii < numProcs; ++ii)
    {
    displacements[ii] = totalNumberOfBlocks * 7;
    recvCounts[ii] = blocksPerProcess[ii] * 7;
    totalNumberOfBlocks += blocksPerProcess[ii];
    }

  // Setup the array to send to the other processes.
  int *localBlockInfo = new int[this->NumberOfInputBlocks*7];
  for (int ii = 0; ii < this->NumberOfInputBlocks; ++ii)
    {
    localBlockInfo[ii*7] = this->InputBlocks[ii]->GetLevel();
    const int *ext;
    // Lets do just the cell extent without overlap.
    // Edges and corners or overlap can cause problems when
    // neighbors are a different level.
    ext = this->InputBlocks[ii]->GetBaseCellExtent();
    for (int jj = 0; jj < 6; ++jj)
      {
      localBlockInfo[ii*7+1 + jj] = ext[jj];
      }
    }
  // Allocate the memory to receive the gathered extents.
  int *gatheredBlockInfo = new int[totalNumberOfBlocks*7];

  // TODO: check for errors ...
  com->AllGatherV(localBlockInfo, gatheredBlockInfo,
                  this->NumberOfInputBlocks*7, recvCounts, 
                  displacements);
  //MPI_Allgatherv((void*)localBlockInfo, this->NumberOfInputBlocks*7, MPI_INT, 
  //(void*)gatheredBlockInfo, recvCounts, displacements, 
  //MPI_INT, *com->GetMPIComm()->GetHandle());

  this->ComputeAndDistributeGhostBlocks(blocksPerProcess,
                                        gatheredBlockInfo,
                                        myProc,
                                        numProcs);
  // Send:
  // Process, extent, data,
  // ...
  // Receive:
  // Process, extent
  // ...


    /*

  // Compute the blocks that need to be sent between all processes.
  //this->ComputeCounts(blocksPerProcess, gatheredBlockInfo, numProcs, myId, 
  //                    sendCounts, recvCounts);
  

    
  // Now get the actual data
  int MPI_Alltoallv(void *sbuf, int *scounts, int *sdisps, MPI_Datatype sdtype, 
              void *rbuf, int *rcounts, int *rdisps, MPI_Datatype rdtype, 
              MPI_Comm comm)   
}
  
  int MPI_Allgatherv ( void *sendbuf, int sendcount, MPI_Datatype sendtype, 
                     void *recvbuf, int *recvcounts, int *displs, 
                    MPI_Datatype recvtype, MPI_Comm comm )
*/
  
  // Determine neighbors we need to send and neighbors to receive.
  
  // AllToAllV.
  
  // Create ghost layer blocks.
  
  delete [] blocksPerProcess;
  delete [] sendCounts;
  delete [] recvCounts;
  delete [] displacements;
  delete [] localBlockInfo;
  delete [] gatheredBlockInfo;
}


//----------------------------------------------------------------------------
// Loop over all blocks from other processes.  Find all local blocks
// that touch the block.
void vtkCTHFragmentConnect::ComputeAndDistributeGhostBlocks(
  int *numBlocksInProc,
  int* blockMetaData,
  int myProc,
  int numProcs)
{
  int requestMsg[8];
  int *ext;
  int bufSize = 0;
  unsigned char* buf = 0;
  int dataSize;
  vtkCTHFragmentConnectBlock* ghostBlock;

  // Loop through the other processes.
  int* blockMetaDataPtr = blockMetaData;
  for (int otherProc = 0; otherProc < numProcs; ++otherProc)
    {
    if (otherProc == myProc)
      {
      this->HandleGhostBlockRequests();
      // Skip the metat data for this process.
      blockMetaDataPtr += + 7*numBlocksInProc[myProc];
      }
    else
      {
      // Loop through the extents in the remote process.
      for (int id = 0;  id < numBlocksInProc[otherProc]; ++id)
        {
        // Request message is (requesting process, block id, required extent)
        requestMsg[0] = myProc;
        requestMsg[1] = id;
        ext = requestMsg + 2;
        // Block meta data is level and base-cell-extent.
        int ghostBlockLevel = blockMetaDataPtr[0];
        int *ghostBlockExt = blockMetaDataPtr+1;

        if (this->ComputeRequiredGhostExtent(ghostBlockLevel, ghostBlockExt, ext))
          {
          this->Controller->Send(requestMsg, 8, otherProc, 708923);
          // Now receive the ghost block.
          dataSize = (ext[1]-ext[0]+1)*(ext[3]-ext[2]+1)*(ext[5]-ext[4]+1);
          if (bufSize < dataSize) 
            {
            if (buf) { delete [] buf;}
            buf = new unsigned char[dataSize];
            bufSize = dataSize;
            }
          this->Controller->Receive(buf, dataSize, otherProc, 433240);
          // Make the ghost block and add it to the grid.
          ghostBlock = new vtkCTHFragmentConnectBlock;
          ghostBlock->InitializeGhostLayer(buf, ext, ghostBlockLevel, 
                                           this->GlobalOrigin, this->RootSpacing,
                                           otherProc, id);
          // Save for deleting.
          this->GhostBlocks.push_back(ghostBlock);
          // Add to grid and connect up neighbors.
          this->AddBlock(ghostBlock);
          }
        // Move to the next block.
        blockMetaDataPtr += 7;
        }
      // Send the message that indicates we have no more requests.
      requestMsg[0] = myProc;
      requestMsg[1] = -1;
      this->Controller->Send(requestMsg, 8, otherProc, 708923);
      }
    }
    
  if (buf)
    {
    delete [] buf;
    }
}


//----------------------------------------------------------------------------
// Keep receiving and filling requests until all processes say they 
// are finished asking for blocks.
void vtkCTHFragmentConnect::HandleGhostBlockRequests()
{
  int requestMsg[8];
  int otherProc;
  int blockId;
  vtkCTHFragmentConnectBlock* block;
  int bufSize = 0;
  unsigned char* buf = 0;
  int dataSize;
  int* ext;

  // We do not receive requests from our own process.
  int remainingProcs = this->Controller->GetNumberOfProcesses() - 1;
  while (remainingProcs != 0)
    {
    this->Controller->Receive(requestMsg, 8, vtkMultiProcessController::ANY_SOURCE, 708923);
    otherProc = requestMsg[0];
    blockId = requestMsg[1];
    if (blockId == -1)
      {
      --remainingProcs;
      }
    else
      {
      // Find the block.
      block = this->InputBlocks[blockId];
      if (block == 0)
        { // Sanity check. This will lock up!
        vtkErrorMacro("Missing block request.");
        return;
        }
      // Crop the block.
      // Now extract the data for the ghost layer.
      ext = requestMsg+2;
      dataSize = (ext[1]-ext[0]+1)*(ext[3]-ext[2]+1)*(ext[5]-ext[4]+1);
      if (bufSize < dataSize) 
        {
        if (buf) { delete [] buf;}
        buf = new unsigned char[dataSize];
        bufSize = dataSize;
        }
      block->ExtractExtent(buf, ext);
      // Send the block.
      this->Controller->Send(buf, dataSize, otherProc, 433240);
      }
    }
  if (buf)
    {
    delete [] buf;
    }
}

//----------------------------------------------------------------------------
// TODO: Try to not get extents supplied by existing overlap.
// Return 1 if we need this ghost block.  Ext is the part we need.
int vtkCTHFragmentConnect::ComputeRequiredGhostExtent(
  int remoteBlockLevel,
  int remoteBaseCellExt[6],
  int neededExt[6])
{
  vtkstd::vector<vtkCTHFragmentConnectBlock*> neighbors;
  int remoteBlockIndex[3];
  int remoteLayerExt[6];


  // Extract the index from the block extent.
  // Lets choose the middle in case the extent has overlap.
  remoteBlockIndex[0] = (remoteBaseCellExt[0]+remoteBaseCellExt[1]) 
                            / (2*this->StandardBlockDimensions[0]);
  remoteBlockIndex[1] = (remoteBaseCellExt[2]+remoteBaseCellExt[3]) 
                            / (2*this->StandardBlockDimensions[1]);
  remoteBlockIndex[2] = (remoteBaseCellExt[4]+remoteBaseCellExt[5]) 
                            / (2*this->StandardBlockDimensions[2]);

  neededExt[0] = neededExt[2] = neededExt[4] = VTK_LARGE_INTEGER;
  neededExt[1] = neededExt[3] = neededExt[5] = -VTK_LARGE_INTEGER;

  // These loops visit all 26 face,edge and corner neighbors.
  int direction[3];
  for (int ix = -1; ix <= 1; ++ix)
    {
    direction[0] = ix;
    for (int iy = -1; iy <= 1; ++iy)
      {
      direction[1] = iy;
      for (int iz = -1; iz <= 1; ++iz)
        {
        direction[2] = iz;
        // Skip the center of the 9x9x9 block.
        if (ix || iy || iz)
          {
          if( this->HasNeighbor(remoteBlockLevel, remoteBlockIndex, direction))
            { // A neighbor exists in this location.
            // Make sure we get ghost cells for this neighbor.
            memcpy(remoteLayerExt, remoteBaseCellExt, 6*sizeof(int));
            if (ix == -1) { remoteLayerExt[1] = remoteLayerExt[0];}
            if (ix == 1)  { remoteLayerExt[0] = remoteLayerExt[1];}
            if (iy == -1) { remoteLayerExt[3] = remoteLayerExt[2];}
            if (iy == 1)  { remoteLayerExt[2] = remoteLayerExt[3];}
            if (iz == -1) { remoteLayerExt[5] = remoteLayerExt[4];}
            if (iz == 1)  { remoteLayerExt[4] = remoteLayerExt[5];}
            // Take the union of all blocks.
            if (neededExt[0] > remoteLayerExt[0]) { neededExt[0] = remoteLayerExt[0];}
            if (neededExt[1] < remoteLayerExt[1]) { neededExt[1] = remoteLayerExt[1];}
            if (neededExt[2] > remoteLayerExt[2]) { neededExt[2] = remoteLayerExt[2];}
            if (neededExt[3] < remoteLayerExt[3]) { neededExt[3] = remoteLayerExt[3];}
            if (neededExt[4] > remoteLayerExt[4]) { neededExt[4] = remoteLayerExt[4];}
            if (neededExt[5] < remoteLayerExt[5]) { neededExt[5] = remoteLayerExt[5];}
            }
          }
        }
      }
    }
  
  if (neededExt[0] > neededExt[1] || 
      neededExt[2] > neededExt[3] || 
      neededExt[4] > neededExt[5])
    {
    return 0;
    }
  return 1;
}



//----------------------------------------------------------------------------
// We need ghostblocks to connect as neighbors too.
void vtkCTHFragmentConnect::AddBlock(vtkCTHFragmentConnectBlock* block)
{
  vtkCTHFragmentLevel* level = this->Levels[block->GetLevel()];
  int dims[3];
  level->GetBlockDimensions(dims);
  block->ComputeBaseExtent(dims);
  this->CheckLevelsForNeighbors(block);
  level->AddBlock(block);
}


//----------------------------------------------------------------------------
int vtkCTHFragmentConnect::RequestData(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *outputVector)
{
  if (this->VolumeFractionArrayName == 0)
    {
    vtkErrorMacro("No volume fraction specified.");
    return 0;
    }

  // get the info objects
  vtkInformation *inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation *outInfo = outputVector->GetInformationObject(0);
    
  vtkPolyData *output = vtkPolyData::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  this->FragmentId = 0;
  this->FragmentVolume = 0.0;
  this->EquivalenceSet->Initialize();

  this->BlockIdArray = vtkIntArray::New();
  this->BlockIdArray->SetName("BlockId");
  this->LevelArray = vtkIntArray::New();
  this->LevelArray->SetName("Level");
  output->GetCellData()->AddArray(this->BlockIdArray);
  output->GetCellData()->AddArray(this->LevelArray);

  this->Mesh = output;
  vtkPoints* points = vtkPoints::New();
  this->Mesh->SetPoints(points);
  points->Delete();
  vtkCellArray* polys = vtkCellArray::New();
  this->Mesh->SetPolys(polys);
  polys->Delete();
  // Id Scalars holds the computed index of the fragment.
  // Make point scalars for now.  Probably use cell scalars in the future.
  vtkIntArray* idScalars = vtkIntArray::New();
  idScalars->SetName("FragmentId");
  output->GetPointData()->SetScalars(idScalars);

  // Keep a counter of which connected component we are upto.
  this->FragmentId = 0;
  this->FragmentVolume = 0.0;

  vtkImageData *inData = vtkImageData::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  if (inData != 0)
    { // Just an image data. Process it directly.
    this->InitializeBlocks(inData);
    int ret = this->ProcessBlock(0);
    this->DeleteAllBlocks();
    this->BlockIdArray->Delete();
    this->BlockIdArray = 0;
    this->LevelArray->Delete();
    this->LevelArray = 0;
    return ret;
    }
    
  // Try AMR
  vtkHierarchicalBoxDataSet *hds=vtkHierarchicalBoxDataSet::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  if (hds)
    {
    this->InitializeBlocks(hds);
    int blockId;
    for (blockId = 0; blockId < this->NumberOfInputBlocks; ++blockId)
      {
      this->ProcessBlock(blockId);
      }
    
    //char tmp[128];
    //sprintf(tmp, "C:/Law/tmp/cthSurface%d.vtp", this->Controller->GetLocalProcessId());
    //this->SaveBlockSurfaces(tmp);
    //sprintf(tmp, "C:/Law/tmp/cthGhost%d.vtp", this->Controller->GetLocalProcessId());
    //this->SaveGhostSurfaces(tmp);
    
    
    this->ResolveEquivalences(idScalars);
    this->GenerateVolumeArray(idScalars, output);
    this->DeleteAllBlocks();
    }

  idScalars->Delete();
  this->BlockIdArray->Delete();
  this->BlockIdArray = 0;
  this->LevelArray->Delete();
  this->LevelArray = 0;

  return 1;
}



//----------------------------------------------------------------------------
int vtkCTHFragmentConnect::ProcessBlock(int blockId)
{
  float middleValue = 127.5;

  vtkCTHFragmentConnectBlock* block = this->InputBlocks[blockId];
  if (block == 0)
    {
    return 0;
    }  
    
  vtkCTHFragmentConnectIterator* xIterator = new vtkCTHFragmentConnectIterator;
  vtkCTHFragmentConnectIterator* yIterator = new vtkCTHFragmentConnectIterator;
  vtkCTHFragmentConnectIterator* zIterator = new vtkCTHFragmentConnectIterator;
  zIterator->Block = block;
  zIterator->VolumeFractionPointer = block->GetBaseVolumeFractionPointer();
  zIterator->FragmentIdPointer = block->GetBaseFragmentIdPointer();

  vtkCTHFragmentConnectRingBuffer *queue = new vtkCTHFragmentConnectRingBuffer;

  // Loop through all the voxels.
  int ix, iy, iz;
  const int *ext;
  int cellIncs[3];
  block->GetCellIncrements(cellIncs);
  ext = block->GetBaseCellExtent();
  for (iz = ext[4]; iz <= ext[5]; ++iz)
    {
    zIterator->Index[2] = iz;
    *yIterator = *zIterator;  
    for (iy = ext[2]; iy <= ext[3]; ++iy)
      {
      yIterator->Index[1] = iy;
      *xIterator = *yIterator; 
      for (ix = ext[0]; ix <= ext[1]; ++ix)
        {
        xIterator->Index[0] = ix;    
        if (*(xIterator->FragmentIdPointer) == -1 && 
            *(xIterator->VolumeFractionPointer) > middleValue) //0.5)
          { // We have a new fragment.
          this->EquivalenceSet->AddEquivalence(this->FragmentId,this->FragmentId);
          // We have to mark every voxel we push on the queue.
          *(xIterator->FragmentIdPointer) = this->FragmentId;
          // There should be no need to clear the queue.
          queue->Push(xIterator);
          this->ConnectFragment(queue);
          // Move to next fragment.
          // Save the volume from the last fragment.
          this->FragmentVolumes->InsertTuple1(this->FragmentId, this->FragmentVolume);
          ++this->FragmentId;
          this->FragmentVolume = 0.0;
          }
        xIterator->VolumeFractionPointer += cellIncs[0];
        xIterator->FragmentIdPointer += cellIncs[0];
        }
      yIterator->VolumeFractionPointer += cellIncs[1];
      yIterator->FragmentIdPointer += cellIncs[1];
      }
    zIterator->VolumeFractionPointer += cellIncs[2];
    zIterator->FragmentIdPointer += cellIncs[2];
    }
  
  delete queue;
  delete xIterator;  
  delete yIterator;  
  delete zIterator;  
    
  return 1;
}


//----------------------------------------------------------------------------
// This method computes the displacement of the corner to place it near a 
// location where the interpolated volume fraction is 0.5.  
// The results are returned as displacmentFactors that scale the basis vectors
// associated with the face.  For the purpose of this computation, 
// These vectors are orthogonal and their 
// magnidutes are half the length of the voxel edges.
// The actual interpolated volume will be warped later.
// The previous version had a problem.  
// Moving along the gradient and being constrained to inside the box
// did not allow us to always hit the 0.5 target.
// This version moves in the direction of the average normal of original
// surface.  It essentially looks at neighbors to determine if original
// voxel surface is flat, an edge, or a corner.
// This method could probably be more efficient. General computation works
// for all gradient directions, but components are either 1 or 0.
void vtkCTHFragmentConnect::ComputeDisplacementFactors(
  vtkCTHFragmentConnectIterator* pointNeighborIterators[8], 
  double displacmentFactors[3])
{
  // DEBUGGING
  // This generates the raw voxel surface when uncommented.
  //displacmentFactors[0] = 0.0;
  //displacmentFactors[1] = 0.0;
  //displacmentFactors[2] = 0.0;
  //return;

  double middleValue = 127.5;

  double v000 = pointNeighborIterators[0]->VolumeFractionPointer[0];
  double v001 = pointNeighborIterators[1]->VolumeFractionPointer[0];
  double v010 = pointNeighborIterators[2]->VolumeFractionPointer[0];
  double v011 = pointNeighborIterators[3]->VolumeFractionPointer[0];
  double v100 = pointNeighborIterators[4]->VolumeFractionPointer[0];
  double v101 = pointNeighborIterators[5]->VolumeFractionPointer[0];
  double v110 = pointNeighborIterators[6]->VolumeFractionPointer[0];
  double v111 = pointNeighborIterators[7]->VolumeFractionPointer[0];
  
  double centerValue = (v000+v001+v010+v011+v100+v101+v110+v111)*0.125;
  double surfaceValue;
  // Compute the gradient after a threshold.
  double t000 = 0.0;
  double t001 = 0.0;
  double t010 = 0.0;
  double t011 = 0.0;
  double t100 = 0.0;
  double t101 = 0.0;
  double t110 = 0.0;
  double t111 = 0.0;
  if (v000 > middleValue) { t000 = 1.0;}
  if (v001 > middleValue) { t001 = 1.0;}
  if (v010 > middleValue) { t010 = 1.0;}
  if (v011 > middleValue) { t011 = 1.0;}
  if (v100 > middleValue) { t100 = 1.0;}
  if (v101 > middleValue) { t101 = 1.0;}
  if (v110 > middleValue) { t110 = 1.0;}
  if (v111 > middleValue) { t111 = 1.0;}

  // We use the gradient ater threshold to choose a direction
  // to move the point.  After considering he discussion about
  // clamping below, we should zeroout iterators that are not
  // face connected (after threshold) to the iterator/voxel
  // that is generating this face.  We do not know which iterator 
  // that is because it was not passed in .......
  
  double g[3]; // Gradient at center.
  g[2] = -t000-t001-t010-t011+t100+t101+t110+t111;
  g[1] = -t000-t001+t010+t011-t100-t101+t110+t111;
  g[0] = -t000+t001-t010+t011-t100+t101-t110+t111;
  // This is unussual but it can happen wioth a checkerboard pattern.
  // We should break the symetry and choose a direction ...
  if (g[0] == 0.0 && g[1] == 0.0 &&  g[2] == 0.0)
    {
    displacmentFactors[0] = displacmentFactors[1] = displacmentFactors[2] = 0.0;
    return;
    }
  // If the center value is above 0.5, then we need to go in the negative gradient direction.
  if (centerValue > middleValue)
    {
    g[0] = -g[0];  
    g[1] = -g[1];  
    g[2] = -g[2];  
    }    
    
  // Scale so that the largest(smallest) component is equal to 0.5(-0.5);
  double max = fabs(g[0]);
  double tmp = fabs(g[1]);
  if (tmp > max) {max = tmp;}
  tmp = fabs(g[2]);
  if (tmp > max) {max = tmp;}
  tmp = 0.5 / max;
  g[0] *= tmp;
  g[1] *= tmp;
  g[2] *= tmp;
  
  // g is on the surface of a unit cube. Compute interpolated surface value.
  surfaceValue = v000*(0.5-g[0])*(0.5-g[1])*(0.5-g[2])
                + v001*(0.5+g[0])*(0.5-g[1])*(0.5-g[2])
                + v010*(0.5-g[0])*(0.5+g[1])*(0.5-g[2])
                + v011*(0.5+g[0])*(0.5+g[1])*(0.5-g[2])
                + v100*(0.5-g[0])*(0.5-g[1])*(0.5+g[2])
                + v101*(0.5+g[0])*(0.5-g[1])*(0.5+g[2])
                + v110*(0.5-g[0])*(0.5+g[1])*(0.5+g[2])
                + v111*(0.5+g[0])*(0.5+g[1])*(0.5+g[2]);
  
  // Compute how far to the surface we must travel.
  double k = (middleValue - centerValue) / (surfaceValue - centerValue);
    // clamping caused artifacts in my test data sphere.
    // What sort of artifacts????  I forget.
    // the test spy data looks ok so lets go ahead with clamping.
    // since we only need to clamp for non manifold surfaces, the
    // ideal solution would be to let the common points diverge.
    // since we use face connectivity, the points will not be connected anyway.
    // We could also keep clean from creating non manifold surfaces.
    // I do not generate the surface in a second pass (when we have 
    // the fragment ids) because it would be too expensive.
  if (k < 0.0) 
    {
    k = 0.0;
    }
  if (k > 1.0) 
    {
    k = 1.0;
    }
  
  // This should give us decent displacement factors.
  displacmentFactors[0] = k * g[0] * 2.0;
  displacmentFactors[1] = k * g[1] * 2.0;
  displacmentFactors[2] = k * g[2] * 2.0;
}


//----------------------------------------------------------------------------
// Pass the non displaced corner location in the point argument.
// It will be modified with the sub voxel displacement.
void vtkCTHFragmentConnect::SubVoxelPositionCorner(
  double* point, 
  vtkCTHFragmentConnectIterator* pointNeighborIterators[8])
{
  double displacementFactors[3];
  this->ComputeDisplacementFactors(pointNeighborIterators, displacementFactors);

  // Find the smallest voxel to size the interpolation.  We use virtual
  // voxels which all have the same size.  Although this 
  // is a simplification of the real interpolation problem, there is some
  // justification for this solution.  A neighboring small voxel should
  // not influence infuence interpolated values beyond its own size.
  // However, this solution can cause the interpolation field to be
  // less smooth.  The alternative is to just interpolate to the center
  // of each voxel.  Although this approach make general interpolation
  // difficult, we are only considering corner, edge and face directions.
  
  // Half edges of the smallest voxel.
  double* hEdge0 = 0;
  double* hEdge1 = 0;
  double* hEdge2 = 0;
  int highestLevel = -1;
  for (int ii = 0; ii < 8; ++ii)
    {
    if (pointNeighborIterators[ii]->Block->GetLevel() > highestLevel)
      {
      highestLevel = pointNeighborIterators[ii]->Block->GetLevel();
      hEdge0 = pointNeighborIterators[ii]->Block->HalfEdges[1];
      hEdge1 = pointNeighborIterators[ii]->Block->HalfEdges[3];
      hEdge2 = pointNeighborIterators[ii]->Block->HalfEdges[5];
      }
    }

  // Apply interpolation factors.
  for (int ii = 0; ii < 3; ++ii)
    {
    // Apply the subvoxel displacements.
    point[ii] += hEdge0[ii]*displacementFactors[0]
        + hEdge1[ii]*displacementFactors[1] 
        + hEdge2[ii]*displacementFactors[2];
    }
}


//----------------------------------------------------------------------------
// The half edges define the coordinate system. They are the vectors
// from the center of a cube to a face (i.e. x, y, z).  Neighbors give 
// the volume fraction values for 18 cells (3,3,2). The completely internal
// face (between cell 4 and 13 is the face to be created. The array is
// ordered (axis0, axis1, axis2).  
// Cell 4 (1,1,0) is the voxel inside the surface.
//
// Now to fix cracks.  If neighbors are higher level, 
// I need to have more than 4 points for a face.
// I am only going to support transitions of 1 level.
void vtkCTHFragmentConnect::CreateFace(
  vtkCTHFragmentConnectIterator* in,
  vtkCTHFragmentConnectIterator* out,
  int axis, int outMaxFlag)
{
  if (in->Block == 0 || in->Block->GetGhostFlag())
    {
    return;
    }

  if (out->Block == 0)
    { // Pad the volume so we can create faces on the outside of the dataset.
    *out = *in;
    if (outMaxFlag)
      {
      ++out->Index[axis];
      }
    else
      {
      --out->Index[axis];
      }
    }

  // Add points to the output.  Create separate points for each triangle.  
  // We can worry about merging points later.
  // FragmentIds get merged later.
  vtkCTHFragmentConnectIterator* cornerNeighbors[8];
  vtkIntArray* idScalars = vtkIntArray::SafeDownCast(this->Mesh->GetPointData()->GetScalars());
  vtkPoints *points = this->Mesh->GetPoints();
  vtkCellArray *polys = this->Mesh->GetPolys();
  vtkIdType quadCornerIds[4];
  vtkIdType quadMidIds[4];
  vtkIdType triPtIds[3];
  vtkIdType startTriId = polys->GetNumberOfCells();

  // Compute the corner and edge points.
  // Store the results in ivars.
  this->ComputeFacePoints(in, out,
                          axis, outMaxFlag);
  // Find the neighbor iterators.
  // Store the results in ivars.
  this->ComputeFaceNeighbors(in, out,
                             axis, outMaxFlag);

  // A word about indexing:
  // face neighbors 2x4x4 indexed face normal axis first, axis1, then axis2.
  // Some may be duplicated if they cover more than one grid element
  // Same order with point neighbors, but they are 2x2x2.
  // Face corners ordered axis1, axis2.
  // Face edge middles are 0: -axis2, 1: -axis1, 2: +axis1, 3: +axis2
  // The rational for this is the order the points are traversed
  // in the neighbor array.

  // Permute the corner neighbors to be xyz ordered.
  int inc0 = 1 << axis;
  int inc1 = 1 << ((axis+1)%3);
  int inc2 = 1 << ((axis+2)%3);
  int i0 = 0;
  int i1 = inc0;
  int i2 =      inc1; 
  int i3 = inc0+inc1; 
  int i4 =           inc2; 
  int i5 = inc0     +inc2; 
  int i6 =      inc1+inc2; 
  int i7 = inc0+inc1+inc2; 

  // Do every thing in coordinate system of face.
  cornerNeighbors[i0] = &(this->FaceNeighbors[0]);
  cornerNeighbors[i1] = &(this->FaceNeighbors[1]);
  cornerNeighbors[i2] = &(this->FaceNeighbors[2]);
  cornerNeighbors[i3] = &(this->FaceNeighbors[3]);
  cornerNeighbors[i4] = &(this->FaceNeighbors[8]);
  cornerNeighbors[i5] = &(this->FaceNeighbors[9]);
  cornerNeighbors[i6] = &(this->FaceNeighbors[10]);
  cornerNeighbors[i7] = &(this->FaceNeighbors[11]);
  this->SubVoxelPositionCorner(this->FaceCornerPoints, cornerNeighbors);
  quadCornerIds[0] = points->InsertNextPoint(this->FaceCornerPoints);
  idScalars->InsertTuple1(quadCornerIds[0], this->FragmentId);
  cornerNeighbors[i0] = &(this->FaceNeighbors[4]);
  cornerNeighbors[i1] = &(this->FaceNeighbors[5]);
  cornerNeighbors[i2] = &(this->FaceNeighbors[6]);
  cornerNeighbors[i3] = &(this->FaceNeighbors[7]);
  cornerNeighbors[i4] = &(this->FaceNeighbors[12]);
  cornerNeighbors[i5] = &(this->FaceNeighbors[13]);
  cornerNeighbors[i6] = &(this->FaceNeighbors[14]);
  cornerNeighbors[i7] = &(this->FaceNeighbors[15]);
  this->SubVoxelPositionCorner(this->FaceCornerPoints+3, cornerNeighbors);
  quadCornerIds[1] = points->InsertNextPoint(this->FaceCornerPoints+3);
  idScalars->InsertTuple1(quadCornerIds[1], this->FragmentId);
  cornerNeighbors[i0] = &(this->FaceNeighbors[16]);
  cornerNeighbors[i1] = &(this->FaceNeighbors[17]);
  cornerNeighbors[i2] = &(this->FaceNeighbors[18]);
  cornerNeighbors[i3] = &(this->FaceNeighbors[19]);
  cornerNeighbors[i4] = &(this->FaceNeighbors[24]);
  cornerNeighbors[i5] = &(this->FaceNeighbors[25]);
  cornerNeighbors[i6] = &(this->FaceNeighbors[26]);
  cornerNeighbors[i7] = &(this->FaceNeighbors[27]);
  this->SubVoxelPositionCorner(this->FaceCornerPoints+6, cornerNeighbors);
  quadCornerIds[2] = points->InsertNextPoint(this->FaceCornerPoints+6);
  idScalars->InsertTuple1(quadCornerIds[2], this->FragmentId);
  cornerNeighbors[i0] = &(this->FaceNeighbors[20]);
  cornerNeighbors[i1] = &(this->FaceNeighbors[21]);
  cornerNeighbors[i2] = &(this->FaceNeighbors[22]);
  cornerNeighbors[i3] = &(this->FaceNeighbors[23]);
  cornerNeighbors[i4] = &(this->FaceNeighbors[28]);
  cornerNeighbors[i5] = &(this->FaceNeighbors[29]);
  cornerNeighbors[i6] = &(this->FaceNeighbors[30]);
  cornerNeighbors[i7] = &(this->FaceNeighbors[31]);
  this->SubVoxelPositionCorner(this->FaceCornerPoints+9, cornerNeighbors);
  quadCornerIds[3] = points->InsertNextPoint(this->FaceCornerPoints+9);
  idScalars->InsertTuple1(quadCornerIds[3], this->FragmentId);

  // Now for the mid edge point if the neighbors on that side are smaller.
  if (this->FaceEdgeFlags[0])
    {
    cornerNeighbors[i0] = &(this->FaceNeighbors[2]);
    cornerNeighbors[i1] = &(this->FaceNeighbors[3]);
    cornerNeighbors[i2] = &(this->FaceNeighbors[4]);
    cornerNeighbors[i3] = &(this->FaceNeighbors[5]);
    cornerNeighbors[i4] = &(this->FaceNeighbors[10]);
    cornerNeighbors[i5] = &(this->FaceNeighbors[11]);
    cornerNeighbors[i6] = &(this->FaceNeighbors[12]);
    cornerNeighbors[i7] = &(this->FaceNeighbors[13]);
    this->SubVoxelPositionCorner(this->FaceEdgePoints, cornerNeighbors);
    quadMidIds[0] = points->InsertNextPoint(this->FaceEdgePoints);
    idScalars->InsertTuple1(quadMidIds[0], this->FragmentId);
    }
  if (this->FaceEdgeFlags[1])
    {
    cornerNeighbors[i0] = &(this->FaceNeighbors[8]);
    cornerNeighbors[i1] = &(this->FaceNeighbors[9]);
    cornerNeighbors[i2] = &(this->FaceNeighbors[10]);
    cornerNeighbors[i3] = &(this->FaceNeighbors[11]);
    cornerNeighbors[i4] = &(this->FaceNeighbors[16]);
    cornerNeighbors[i5] = &(this->FaceNeighbors[17]);
    cornerNeighbors[i6] = &(this->FaceNeighbors[18]);
    cornerNeighbors[i7] = &(this->FaceNeighbors[19]);
    this->SubVoxelPositionCorner(this->FaceEdgePoints+3, cornerNeighbors);
    quadMidIds[1] = points->InsertNextPoint(this->FaceEdgePoints+3);
    idScalars->InsertTuple1(quadMidIds[1], this->FragmentId);
    }
  if (this->FaceEdgeFlags[2])
    {
    cornerNeighbors[i0] = &(this->FaceNeighbors[12]);
    cornerNeighbors[i1] = &(this->FaceNeighbors[13]);
    cornerNeighbors[i2] = &(this->FaceNeighbors[14]);
    cornerNeighbors[i3] = &(this->FaceNeighbors[15]);
    cornerNeighbors[i4] = &(this->FaceNeighbors[20]);
    cornerNeighbors[i5] = &(this->FaceNeighbors[21]);
    cornerNeighbors[i6] = &(this->FaceNeighbors[22]);
    cornerNeighbors[i7] = &(this->FaceNeighbors[23]);
    this->SubVoxelPositionCorner(this->FaceEdgePoints+6, cornerNeighbors);
    quadMidIds[2] = points->InsertNextPoint(this->FaceEdgePoints+6);
    idScalars->InsertTuple1(quadMidIds[2], this->FragmentId);
    }
  if (this->FaceEdgeFlags[3])
    {
    cornerNeighbors[i0] = &(this->FaceNeighbors[18]);
    cornerNeighbors[i1] = &(this->FaceNeighbors[19]);
    cornerNeighbors[i2] = &(this->FaceNeighbors[20]);
    cornerNeighbors[i3] = &(this->FaceNeighbors[21]);
    cornerNeighbors[i4] = &(this->FaceNeighbors[26]);
    cornerNeighbors[i5] = &(this->FaceNeighbors[27]);
    cornerNeighbors[i6] = &(this->FaceNeighbors[28]);
    cornerNeighbors[i7] = &(this->FaceNeighbors[29]);
    this->SubVoxelPositionCorner(this->FaceEdgePoints+9, cornerNeighbors);
    quadMidIds[3] = points->InsertNextPoint(this->FaceEdgePoints+9);
    idScalars->InsertTuple1(quadMidIds[3], this->FragmentId);
    }

  // Now there are 9 possibilities 
  // (10 if you count the two ways to triangulate the simple quad).
  // No edges, $ cases with one mid point, 4 cases with two mid points.
  // That is all because the face is always the smallest of the two in/out voxels.
  int caseIdx = this->FaceEdgeFlags[0] | (this->FaceEdgeFlags[1] << 1)
                  | (this->FaceEdgeFlags[2] << 2) | (this->FaceEdgeFlags[3] << 3);
  
  //c2 e3 c3
  //e1    e2
  //c0 e0 c1

  switch (caseIdx)
    {
    // One edge point
    case 1:
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadCornerIds[2];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadCornerIds[3];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadCornerIds[1];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 2:
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadCornerIds[3];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadCornerIds[1];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadCornerIds[0];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 4:
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadCornerIds[0];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadCornerIds[2];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadCornerIds[3];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 8:
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadCornerIds[1];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadCornerIds[0];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadCornerIds[2];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      break;
    // Two adjacent edge points
    case 3: // e0 and e1
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadMidIds[1];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadMidIds[0];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadCornerIds[3];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadCornerIds[1];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 5: // e0 and e2
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadMidIds[0];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadMidIds[2];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadCornerIds[3];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadCornerIds[2];
      triPtIds[2] = quadMidIds[0];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 10: // e1 and e3
      triPtIds[0] = quadCornerIds[2];
      triPtIds[1] = quadMidIds[3];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadMidIds[1];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadCornerIds[1];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadCornerIds[0];
      triPtIds[2] = quadMidIds[1];
      polys->InsertNextCell(3, triPtIds);
      break;
    case 12: // e2 and e3
      triPtIds[0] = quadCornerIds[3];
      triPtIds[1] = quadMidIds[2];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadMidIds[3];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[1];
      triPtIds[1] = quadCornerIds[0];
      triPtIds[2] = quadMidIds[2];
      polys->InsertNextCell(3, triPtIds);
      triPtIds[0] = quadCornerIds[0];
      triPtIds[1] = quadCornerIds[2];
      triPtIds[2] = quadMidIds[3];
      polys->InsertNextCell(3, triPtIds);
      break;
    // No edges.
    // For a simple quad, choose a triangulation.
    case 0:
      {
      // Compute length of diagonals (squared)
      // This will help us decide which way to split up the quad into triangles.
      double tmp;
      double d0011 = 0.0;
      double d0110 = 0.0;
      double *pt00 = this->FaceCornerPoints;
      double *pt01 = this->FaceCornerPoints+3;
      double *pt10 = this->FaceCornerPoints+6;
      double *pt11 = this->FaceCornerPoints+9;
      for (int ii = 0; ii < 3; ++ii)
        {
        tmp = pt00[ii]-pt11[ii];
        d0011 += tmp*tmp;
        tmp = pt01[ii]-pt10[ii];
        d0110 += tmp*tmp;
        }
      if (d0011 < d0110)
        {   
        triPtIds[0] = quadCornerIds[0];
        triPtIds[1] = quadCornerIds[1];
        triPtIds[2] = quadCornerIds[3];
        polys->InsertNextCell(3, triPtIds);
        triPtIds[0] = quadCornerIds[0];
        triPtIds[1] = quadCornerIds[3];
        triPtIds[2] = quadCornerIds[2];
        polys->InsertNextCell(3, triPtIds);
        }
      else
        {
        triPtIds[0] = quadCornerIds[1];
        triPtIds[1] = quadCornerIds[3];
        triPtIds[2] = quadCornerIds[2];
        polys->InsertNextCell(3, triPtIds);
        triPtIds[0] = quadCornerIds[2];
        triPtIds[1] = quadCornerIds[0];
        triPtIds[2] = quadCornerIds[1];
        polys->InsertNextCell(3, triPtIds);
        }
      break;
      }
    default:
      vtkErrorMacro("Unexpected edge case.");
    }

  // Cell data attributes for debugging.
  vtkIdType numTris = polys->GetNumberOfCells() - startTriId;

  for (vtkIdType ii = 0; ii < numTris; ++ii)
    {
    this->LevelArray->InsertNextValue(in->Block->GetLevel());
    //this->BlockIdArray->InsertNextValue(iterator->Block->GetBlockId());
    this->BlockIdArray->InsertNextValue(in->Block->LevelBlockId);
    }
}

//----------------------------------------------------------------------------
// Computes the face and edge middle points of the shared contact face
// between the two iterators.
void vtkCTHFragmentConnect::ComputeFacePoints(
  vtkCTHFragmentConnectIterator* in,
  vtkCTHFragmentConnectIterator* out,
  int axis, int outMaxFlag)
{
  vtkCTHFragmentConnectIterator* smaller;
  double* origin;
  double* spacing;
  int maxFlag;
  int axis1 = (axis+1)%3;
  int axis2 = (axis+2)%3;
  
  // We create the smaller face of the two voxels.
  smaller = in;
  // A bit confusing.  If out iterator is max, then the face of out is min.
  maxFlag = outMaxFlag;
  if (in->Block->GetLevel() < out->Block->GetLevel())
    {
    smaller = out;
    maxFlag = !outMaxFlag;
    }
  
  origin = smaller->Block->GetOrigin();
  spacing = smaller->Block->GetSpacing();
  double halfSpacing[3];
  double faceOrigin[3];
  halfSpacing[0] = spacing[0] * 0.5;
  halfSpacing[1] = spacing[1] * 0.5;
  halfSpacing[2] = spacing[2] * 0.5;
  // find the origin of the voxel.
  faceOrigin[0] = origin[0] + (double)(smaller->Index[0]) * spacing[0];
  faceOrigin[1] = origin[1] + (double)(smaller->Index[1]) * spacing[1];
  faceOrigin[2] = origin[2] + (double)(smaller->Index[2]) * spacing[2];
  // Move the voxel origin to the face origin.
  if (maxFlag)
    {
    faceOrigin[axis] += spacing[axis];
    }

  // Now compute all of the corner points.
  // 6 9
  // 0 3
  // First set them all to the origin.
  this->FaceCornerPoints[0] = this->FaceCornerPoints[3] = 
    this->FaceCornerPoints[6] = this->FaceCornerPoints[9] = faceOrigin[0];
  this->FaceCornerPoints[1] = this->FaceCornerPoints[4] = 
    this->FaceCornerPoints[7] = this->FaceCornerPoints[10] = faceOrigin[1];
  this->FaceCornerPoints[2] = this->FaceCornerPoints[5] = 
    this->FaceCornerPoints[8] = this->FaceCornerPoints[11] = faceOrigin[2];
  // Now offset them to the corners.
  this->FaceCornerPoints[3+axis1] += spacing[axis1];
  this->FaceCornerPoints[9+axis1] += spacing[axis1];
  this->FaceCornerPoints[6+axis2] += spacing[axis2];
  this->FaceCornerPoints[9+axis2] += spacing[axis2];

  // Now do the same for the edge points
  //   3
  // 1   2
  //   0
  // First set them all to the origin.
  this->FaceEdgePoints[0] = this->FaceEdgePoints[3] = 
    this->FaceEdgePoints[6] = this->FaceEdgePoints[9] = faceOrigin[0];
  this->FaceEdgePoints[1] = this->FaceEdgePoints[4] = 
    this->FaceEdgePoints[7] = this->FaceEdgePoints[10] = faceOrigin[1];
  this->FaceEdgePoints[2] = this->FaceEdgePoints[5] = 
    this->FaceEdgePoints[8] = this->FaceEdgePoints[11] = faceOrigin[2];  
  // Now offset the points to the middle of the edges.
  this->FaceEdgePoints[axis1] += halfSpacing[axis1];
  this->FaceEdgePoints[9+axis1] += halfSpacing[axis1];
  this->FaceEdgePoints[6+axis1] += spacing[axis1];
  this->FaceEdgePoints[3+axis2] += halfSpacing[axis2];
  this->FaceEdgePoints[6+axis2] += halfSpacing[axis2];
  this->FaceEdgePoints[9+axis2] += spacing[axis2];
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::ComputeFaceNeighbors(
  vtkCTHFragmentConnectIterator* in,
  vtkCTHFragmentConnectIterator* out,
  int axis, int outMaxFlag)
{
  int axis1 = (axis+1)%3;
  int axis2 = (axis+2)%3;

  // Neighbors are one level higher than the smallest voxel.
  int faceLevel;
  int faceIndex[3];
  // We need to find the smallest face (largest level) of the two iterators.
  if (in->Block->GetLevel() > out->Block->GetLevel())
    {
    faceLevel = in->Block->GetLevel() + 1;
    faceIndex[0] = in->Index[0];
    faceIndex[1] = in->Index[1];
    faceIndex[2] = in->Index[2];
    if (outMaxFlag)
      {
      faceIndex[axis] += 1;
      }
    }
  else
    {
    faceLevel = out->Block->GetLevel() + 1;
    faceIndex[0] = out->Index[0];
    faceIndex[1] = out->Index[1];
    faceIndex[2] = out->Index[2];
    if ( ! outMaxFlag)
      {
      faceIndex[axis] += 1;
      }
    }
  faceIndex[0] = faceIndex[0] << 1;
  faceIndex[1] = faceIndex[1] << 1;
  faceIndex[2] = faceIndex[2] << 1;

  // The center four on each side of the face are always the same.
  // The face is the smallest of in and out, so there is no possibility
  // for subdivision.
  if (outMaxFlag)
    {
    this->FaceNeighbors[10] = this->FaceNeighbors[12] =
       this->FaceNeighbors[18] = this->FaceNeighbors[20] = *in;
    this->FaceNeighbors[11] = this->FaceNeighbors[13] =
       this->FaceNeighbors[19] = this->FaceNeighbors[21] = *out;
    }
  else
    {
    this->FaceNeighbors[10] = this->FaceNeighbors[12] =
       this->FaceNeighbors[18] = this->FaceNeighbors[20] = *out;
    this->FaceNeighbors[11] = this->FaceNeighbors[13] =
       this->FaceNeighbors[19] = this->FaceNeighbors[21] = *in;
    }

  // Ok, we have 24 neighbors to compute.
  // How can we do this efficiently?
  // faceIndex and faceLevel here are actually neighborIndex and neighborLevel.
  // Face index starts at (1,1,1)
  // increments: 1, 2, 8
  // Start at the corner and march around the edges.
  faceIndex[axis1] -= 1;
  faceIndex[axis2] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+1, this->FaceNeighbors+11);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+3, this->FaceNeighbors+11);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+5, this->FaceNeighbors+11);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+7, this->FaceNeighbors+11);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+15, this->FaceNeighbors+11);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+23, this->FaceNeighbors+11);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+31, this->FaceNeighbors+11);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+29, this->FaceNeighbors+11);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+27, this->FaceNeighbors+11);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+25, this->FaceNeighbors+11);
  faceIndex[axis2] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+17, this->FaceNeighbors+11);
  faceIndex[axis2] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+9, this->FaceNeighbors+11);
  //Now for the other side (min axis).
  faceIndex[axis2] -= 1;
  faceIndex[axis] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+0, this->FaceNeighbors+10);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+2, this->FaceNeighbors+10);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+4, this->FaceNeighbors+10);
  faceIndex[axis1] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+6, this->FaceNeighbors+10);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+14, this->FaceNeighbors+10);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+22, this->FaceNeighbors+10);
  faceIndex[axis2] += 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+30, this->FaceNeighbors+10);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+28, this->FaceNeighbors+10);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+26, this->FaceNeighbors+10);
  faceIndex[axis1] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+24, this->FaceNeighbors+10);
  faceIndex[axis2] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+16, this->FaceNeighbors+10);
  faceIndex[axis2] -= 1;
  this->FindNeighbor(faceIndex, faceLevel, this->FaceNeighbors+8, this->FaceNeighbors+10);
}


//----------------------------------------------------------------------------
// Find an iterator from an index,level and a neighbor reference iterator.
void vtkCTHFragmentConnect::FindNeighbor(
  int faceIdx[3], int faceLevel, 
  vtkCTHFragmentConnectIterator* neighbor,
  vtkCTHFragmentConnectIterator* reference)
{
  // Convert the index to the level of the reference block.
  int neighborIdx[3];
  int refLevel = reference->Block->GetLevel();
  
  neighborIdx[0] = faceIdx[0] >> (faceLevel-refLevel);
  neighborIdx[1] = faceIdx[1] >> (faceLevel-refLevel);
  neighborIdx[2] = faceIdx[2] >> (faceLevel-refLevel);

  // The index might point to the reference iterator.
  if (neighborIdx[0] == reference->Index[0] && 
      neighborIdx[1] == reference->Index[1] &&
      neighborIdx[2] == reference->Index[2])
    {
    *neighbor = *reference;
    return;
    }

  // Find the block the neighbor is in.
  vtkCTHFragmentConnectBlock* referenceBlock = reference->Block;
  const int* ext;
  ext = referenceBlock->GetBaseCellExtent();

  // Check each axis and direction for the extent leaving the bounds.
  for (int axis = 0; axis < 3; ++axis)
    {
    int minIdx = 2*axis;
    int maxIdx = minIdx + 1;
    for (int max = 0; max < 2; ++max)
      {
      if ((max == 0 && neighborIdx[axis] < ext[minIdx]) || 
          (max == 1 && neighborIdx[axis] > ext[maxIdx]))
        { // Index is in neighboring block.  Find the block.
        vtkCTHFragmentConnectBlock* block;
        int num, idx;
        num = referenceBlock->GetNumberOfFaceNeighbors(2*axis+max);
        for (idx = 0; idx < num; ++idx)
          {
          block = referenceBlock->GetFaceNeighbor(2*axis+max, idx);
          // Is this the one?
          int tmpIdx[3];
          int level = block->GetLevel();
          tmpIdx[0] = faceIdx[0] >> (faceLevel-level);
          tmpIdx[1] = faceIdx[1] >> (faceLevel-level);
          tmpIdx[2] = faceIdx[2] >> (faceLevel-level);
          const int* tmpExt = block->GetBaseCellExtent();
          if (tmpIdx[axis] >= tmpExt[minIdx] && tmpIdx[axis] <= tmpExt[maxIdx])
            { // yes
            referenceBlock = block;
            ext = tmpExt;
            neighborIdx[0] = tmpIdx[0];
            neighborIdx[1] = tmpIdx[1];
            neighborIdx[2] = tmpIdx[2];
            // Terminate the search of this direction (block has changed!)
            num = 0;
            }
          }
        }
      }
    }

  // We have a block
  // clamp the neighbor index to pad the volume
  if (neighborIdx[0] < ext[0]) { neighborIdx[0] = ext[0];}
  if (neighborIdx[0] > ext[1]) { neighborIdx[0] = ext[1];}
  if (neighborIdx[1] < ext[2]) { neighborIdx[1] = ext[2];}
  if (neighborIdx[1] > ext[3]) { neighborIdx[1] = ext[3];}
  if (neighborIdx[2] < ext[4]) { neighborIdx[2] = ext[4];}
  if (neighborIdx[2] > ext[5]) { neighborIdx[2] = ext[5];}

  neighbor->Block = referenceBlock;
  neighbor->Index[0] = neighborIdx[0];
  neighbor->Index[1] = neighborIdx[1];
  neighbor->Index[2] = neighborIdx[2];
  const int *incs = referenceBlock->GetCellIncrements();
  int offset = (neighborIdx[0]- ext[0])*incs[0]
             + (neighborIdx[1]- ext[2])*incs[1]
             + (neighborIdx[2]- ext[4])*incs[2];
  neighbor->FragmentIdPointer = referenceBlock->GetBaseFragmentIdPointer() + offset;
  neighbor->VolumeFractionPointer = referenceBlock->GetBaseVolumeFractionPointer() + offset;
}







//----------------------------------------------------------------------------
// This is for getting the neighbors necessary to place face points.
// If the next iterator is out of bounds, the last iterator is duplicated.
void vtkCTHFragmentConnect::GetNeighborIteratorPad(
  vtkCTHFragmentConnectIterator* next,
  vtkCTHFragmentConnectIterator* iterator, 
  int axis0, int maxFlag0,
  int axis1, int maxFlag1,
  int axis2, int maxFlag2)
{
  if (iterator->VolumeFractionPointer == 0)
    {
    cerr << "Error empty input block.  Cannot find neighbor.\n";
    *next = *iterator;
    return;
    }
  this->GetNeighborIterator(next, iterator, 
                            axis0, maxFlag0,
                            axis1, maxFlag1,
                            axis2, maxFlag2);

  if (next->VolumeFractionPointer == 0)
    { // Next is out of bounds. Duplicate the last iterator to get a values.
    *next = *iterator;
    if (maxFlag0)
      {
      ++next->Index[axis0];
      }
    else
      {
      --next->Index[axis0];
      }
    }
}

//----------------------------------------------------------------------------
// Find the neighboring voxel and return results in "next".
// Neighbor could be in another block.
// The neighbor is along axis0 in the maxFlag0 direction.
// If the neighbor is in a higher level, then maxFlag1 and maxFlag2
// are used to determine which subvoxel to return.
// Otherwise, these arguments have no effect.
// This last feature is used to find iterators around a point for positioning.
void vtkCTHFragmentConnect::GetNeighborIterator(
  vtkCTHFragmentConnectIterator* next,
  vtkCTHFragmentConnectIterator* iterator, 
  int axis0, int maxFlag0,
  int axis1, int maxFlag1,
  int axis2, int maxFlag2)
{
  if (iterator->Block == 0)
    { // No input, no output.  This should not happen.
    vtkWarningMacro("Can not find neighbor for NULL block.");
    *next = *iterator;
    return;
    }
    
  const int *ext;
  ext = iterator->Block->GetBaseCellExtent();
  int incs[3];
  iterator->Block->GetCellIncrements(incs);

  if (maxFlag0 && iterator->Index[axis0] < ext[2*axis0+1])
    { // Neighbor is inside this block.
    *next = *iterator;
    next->Index[axis0] += 1;
    next->VolumeFractionPointer += incs[axis0];
    next->FragmentIdPointer += incs[axis0];
    return;
    }
  if (!maxFlag0 && iterator->Index[axis0] > ext[2*axis0])
    { // Neighbor is inside this block.
    *next = *iterator;
    next->Index[axis0] -= 1;
    next->VolumeFractionPointer -= incs[axis0];
    next->FragmentIdPointer -= incs[axis0];
    return;
    }
  // Look for a neighboring block.
  vtkCTHFragmentConnectBlock* block;
  int num, idx;
  num = iterator->Block->GetNumberOfFaceNeighbors(2*axis0+maxFlag0);
  for (idx = 0; idx < num; ++idx)
    {
    block = iterator->Block->GetFaceNeighbor(2*axis0+maxFlag0, idx);
    // Convert index into new block level.
    next->Index[0] = iterator->Index[0];
    next->Index[1] = iterator->Index[1];
    next->Index[2] = iterator->Index[2];
    // When moving to the right, increment before changing level.

    // Negative shifts do not behave as expected!!!!
    if (iterator->Block->GetLevel() > block->GetLevel())
      { // Going to a lower level.
      if (maxFlag0)
        {
        next->Index[axis0] += 1;
        next->Index[axis0] = next->Index[axis0] >> (iterator->Block->GetLevel()-block->GetLevel());
        }
      else
        {
        next->Index[axis0] = next->Index[axis0] >> (iterator->Block->GetLevel()-block->GetLevel());
        next->Index[axis0] -= 1;
        }
      // maxFlags for axis1 and 2 do nothing in this case.
      next->Index[axis1] = next->Index[axis1] >> (iterator->Block->GetLevel()-block->GetLevel());
      next->Index[axis2] = next->Index[axis2] >> (iterator->Block->GetLevel()-block->GetLevel());
      }
    else if (iterator->Block->GetLevel() < block->GetLevel())
      { // Going to a higher level.
      if (maxFlag0)
        {
        next->Index[axis0] += 1;
        next->Index[axis0] = next->Index[axis0] << (block->GetLevel()-iterator->Block->GetLevel());
        }
      else
        {
        next->Index[axis0] = next->Index[axis0] << (block->GetLevel()-iterator->Block->GetLevel());
        next->Index[axis0] -= 1;
        }
      if (maxFlag1)
        {
        next->Index[axis1] = ((next->Index[axis1]+1) << (block->GetLevel()-iterator->Block->GetLevel())) - 1;
        }
      else
        {
        next->Index[axis1] = next->Index[axis1] << (block->GetLevel()-iterator->Block->GetLevel());
        }
      if (maxFlag2)
        {
        next->Index[axis2] = ((next->Index[axis2]+1) << (block->GetLevel()-iterator->Block->GetLevel())) - 1;
        }
      else
        {
        next->Index[axis2] = next->Index[axis2] << (block->GetLevel()-iterator->Block->GetLevel());
        }
      }
    else
      { // same level: just increment/decrement the index.
      if (maxFlag0)
        {
        next->Index[axis0] += 1;
        }
      else
        {
        next->Index[axis0] -= 1;
        }
      }

    ext = block->GetBaseCellExtent();
    if ((ext[0] <= next->Index[0] && next->Index[0] <= ext[1]) && 
        (ext[2] <= next->Index[1] && next->Index[1] <= ext[3]) &&
        (ext[4] <= next->Index[2] && next->Index[2] <= ext[5]))
      { // Neighbor voxel is in this block.
      next->Block = block;
      block->GetCellIncrements(incs);
      int offset = (next->Index[0]-ext[0])*incs[0]
                    + (next->Index[1]-ext[2])*incs[1]
                    + (next->Index[2]-ext[4])*incs[2];
      next->VolumeFractionPointer = block->GetBaseVolumeFractionPointer() + offset;
      next->FragmentIdPointer = block->GetBaseFragmentIdPointer() + offset;
      return;
      }
    }
  // No neighbors contain this next index.
  next->Initialize();
}



//----------------------------------------------------------------------------
// Depth first search marking voxels.
// This extracts faces at the same time.
// This is called only when the voxel is part of a fragment.
// I tried to create a generic API to replace the hard coded conditional ifs.
void vtkCTHFragmentConnect::ConnectFragment(vtkCTHFragmentConnectRingBuffer * queue)
{
  while (queue->GetSize())
    {
    // Get the next voxel/iterator to search.
    vtkCTHFragmentConnectIterator iterator;
    queue->Pop(&iterator);
    
    // Lets integrate volume when we remove the iterator from the queue.
    // We could also do it wehn we add the iterator to the queue, but
    // the adds occur in so many places.
    if (iterator.Block->GetGhostFlag() == 0)
      {
      double* spacing = iterator.Block->GetSpacing();
      this->FragmentVolume += spacing[0] * spacing[1] * spacing[2]
             * (double)(*(iterator.VolumeFractionPointer)) / 255.0;
      }
      
    double middleValue = 127.5;

    // Create another iterator on the stack for recursion.
    vtkCTHFragmentConnectIterator next;

    // Look at the face connected neighbors and recurse.
    // We are not on the border and volume fraction of neighbor is high and
    // we have not visited the voxel yet.
    for (int ii = 0; ii < 3; ++ii)
      {
      // I would make these variables to make the computation more clear, 
      // but they would accumulate on the stack.
      //axis0 = ii;
      //axis1 = (ii+1)%3;
      //axis2 = (ii+2)%3;
      //idxMin = 2*ii;
      //idxMax = 2*ii+1this->IndexMax+1;
      // "Left"/min
      this->GetNeighborIterator(&next, &iterator, ii,0, (ii+1)%3,0, (ii+2)%3,0);

      if (next.VolumeFractionPointer == 0 || next.VolumeFractionPointer[0] < middleValue)
        {
        // Neighbor is outside of fragment.  Make a face.
        this->CreateFace(&iterator, &next, ii, 0);
        }    
      else if (next.FragmentIdPointer[0] == -1)
        { // We have not visited this neighbor yet. Mark the voxel and recurse.
        *(next.FragmentIdPointer) = this->FragmentId;
        queue->Push(&next);
        }
      else
        { // The last case is that we have already visited this voxel and it
        // is in the same fragment.
        this->AddEquivalence(&iterator, &next);
        }

      // Handle the case when the new iterator is a higher level.
      // We need to loop over all the faces of the higher level that touch this face.
      // We will restrict our case to 4 neighbors (max difference in levels is 1).
      // If level skip, things should still work OK. Biggest issue is holes in surface.
      // This also sort of assumes that at most one other block touches this face.
      // Holes might appear if this is not true.
      if (next.Block && next.Block->GetLevel() > iterator.Block->GetLevel())
        {
        vtkCTHFragmentConnectIterator next2;
        // Take the first neighbor found and move +Y
        this->GetNeighborIterator(&next2, &next, (ii+1)%3,1, (ii+2)%3,0, ii,0);
        if (next2.VolumeFractionPointer == 0 || next2.VolumeFractionPointer[0] < middleValue)
          {
          // Neighbor is outside of fragment.  Make a face.
          this->CreateFace(&iterator, &next2, ii, 0);
          }    
        else if (next2.FragmentIdPointer[0] == -1)
          { // We have not visited this neighbor yet. Mark the voxel and recurse.
         *(next2.FragmentIdPointer) = this->FragmentId;
          queue->Push(&next2);
          }
        else
          { // The last case is that we have already visited this voxel and it
          // is in the same fragment.
          this->AddEquivalence(&next2, &next);
          }
        // Take the fist iterator found and move +Z
        this->GetNeighborIterator(&next2, &next, (ii+2)%3,1, ii,0, (ii+1)%3,0);
        if (next2.VolumeFractionPointer == 0 || next2.VolumeFractionPointer[0] < middleValue)
          {
          // Neighbor is outside of fragment.  Make a face.
          this->CreateFace(&iterator, &next2, ii, 0);
          }    
        else if (next2.FragmentIdPointer[0] == -1)
          { // We have not visited this neighbor yet. Mark the voxel and recurse.
          *(next2.FragmentIdPointer) = this->FragmentId;
          queue->Push(&next2);
          }
        else
          { // The last case is that we have already visited this voxel and it
          // is in the same fragment.
          this->AddEquivalence(&next2, &next);
          }
        // To get the +Y+Z start with the +Z iterator and move +Y put results in "next"
        if (next2.Block)
          {
          this->GetNeighborIterator(&next, &next2, (ii+1)%3,1, (ii+2)%3,0, ii,0);
          if (next.VolumeFractionPointer == 0 || next.VolumeFractionPointer[0] < middleValue)
            {
            // Neighbor is outside of fragment.  Make a face.
            this->CreateFace(&iterator, &next, ii, 0);
            }    
          else if (next.FragmentIdPointer[0] == -1)
            { // We have not visited this neighbor yet. Mark the voxel and recurse.
            *(next.FragmentIdPointer) = this->FragmentId;
            queue->Push(&next);
            }
          else
            { // The last case is that we have already visited this voxel and it
            // is in the same fragment.
            this->AddEquivalence(&next2, &next);
            }
          }
        }

      // "Right"/max
      this->GetNeighborIterator(&next, &iterator, ii,1, (ii+1)%3,0, (ii+2)%3,0);
      if (next.VolumeFractionPointer == 0 || next.VolumeFractionPointer[0] < middleValue)
        { // Neighbor is outside of fragment.  Make a face.
        this->CreateFace(&iterator, &next, ii, 1);
        }
      else if (next.FragmentIdPointer[0] == -1)
        { // We have not visited this neighbor yet. Mark the voxel and recurse.
        *(next.FragmentIdPointer) = this->FragmentId;
        queue->Push(&next);
        }
      else
        { // The last case is that we have already visited this voxel and it
        // is in the same fragment.
        this->AddEquivalence(&iterator, &next);
        }
      // Same case as above with the same logic to visit the
      // four smaller cells that touch this face of the current block.
      if (next.Block && next.Block->GetLevel() > iterator.Block->GetLevel())
        {
        vtkCTHFragmentConnectIterator next2;
        // Take the first neighbor found and move +Y
        this->GetNeighborIterator(&next2, &next, (ii+1)%3,1, (ii+2)%3,0, ii,0);
        if (next2.VolumeFractionPointer == 0 || next2.VolumeFractionPointer[0] < middleValue)
          {
          // Neighbor is outside of fragment.  Make a face.
          this->CreateFace(&iterator, &next2, ii, 1);
          }    
        else if (next2.FragmentIdPointer[0] == -1)
          { // We have not visited this neighbor yet. Mark the voxel and recurse.
          *(next2.FragmentIdPointer) = this->FragmentId;
          queue->Push(&next2);
          }
        else
          { // The last case is that we have already visited this voxel and it
          // is in the same fragment.
          this->AddEquivalence(&next2, &next);
          }
        // Take the fist iterator found and move +Z
        this->GetNeighborIterator(&next2, &next, (ii+2)%3,1, ii,0, (ii+1)%3,0);
        if (next2.VolumeFractionPointer == 0 || next2.VolumeFractionPointer[0] < middleValue)
          {
          // Neighbor is outside of fragment.  Make a face.
          this->CreateFace(&iterator, &next2, ii, 1);
          }    
        else if (next2.FragmentIdPointer[0] == -1)
          { // We have not visited this neighbor yet. Mark the voxel and recurse.
          *(next2.FragmentIdPointer) = this->FragmentId;
          queue->Push(&next2);
          }
        else
          { // The last case is that we have already visited this voxel and it
          // is in the same fragment.
          this->AddEquivalence(&next2, &next);
          }
        // To get the +Y+Z start with the +Z iterator and move +Y put results in "next"
        if (next2.Block)
          {
          this->GetNeighborIterator(&next, &next2, (ii+1)%3,1, (ii+2)%3,0, ii,0);
          if (next.VolumeFractionPointer == 0 || next.VolumeFractionPointer[0] < middleValue)
            {
            // Neighbor is outside of fragment.  Make a face.
            this->CreateFace(&iterator, &next, ii, 1);
            }    
          else if (next.FragmentIdPointer[0] == -1)
            { // We have not visited this neighbor yet. Mark the voxel and recurse.
            *(next.FragmentIdPointer) = this->FragmentId;
            queue->Push(&next);
            }
          else
            { // The last case is that we have already visited this voxel and it
            // is in the same fragment.
            this->AddEquivalence(&next2, &next);
            }
          }
        }
      }
    }
}





//----------------------------------------------------------------------------
int vtkCTHFragmentConnect::FillInputPortInformation(int port, vtkInformation *info)
{
  if(!this->Superclass::FillInputPortInformation(port,info))
    {
    return 0;
    }
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataObject");
  
  return 1;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

}


//----------------------------------------------------------------------------
// Save out the blocks surfaces so we can se what is creating so many ghost cells.
void vtkCTHFragmentConnect::SaveBlockSurfaces(const char* fileName)
{
  vtkPolyData* pd = vtkPolyData::New();
  vtkPoints* pts = vtkPoints::New();
  vtkCellArray* faces = vtkCellArray::New();
  vtkIntArray* idArray = vtkIntArray::New();
  vtkIntArray* levelArray = vtkIntArray::New();
  vtkCTHFragmentConnectBlock* block;
  const int *ext;
  vtkIdType corners[8];
  vtkIdType face[4];
  double pt[3];
  int level;
  int levelId;
  int ii;
  
  for (ii = 0; ii < this->NumberOfInputBlocks; ++ii)
    {
    block = this->InputBlocks[ii];
    ext = block->GetBaseCellExtent();
    // Ghost blocks do not have spacing.
    //double *spacing;
    //spacing = block->GetSpacing();
    double spacing[3];
    level = block->GetLevel();
    spacing[0] = this->RootSpacing[0] / (double)(1 << level);
    spacing[1] = this->RootSpacing[1] / (double)(1 << level);
    spacing[2] = this->RootSpacing[2] / (double)(1 << level);
    //spacing[0] = spacing[1] = spacing[2] = 1.0 / (double)(1 << level);
    levelId = block->LevelBlockId;
    // Insert the points.
    pt[0] = ext[0]*spacing[0] + this->GlobalOrigin[0];
    pt[1] = ext[2]*spacing[1] + this->GlobalOrigin[1];
    pt[2] = ext[4]*spacing[2] + this->GlobalOrigin[2];
    corners[0] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing[0] + this->GlobalOrigin[0];
    pt[1] = ext[2]*spacing[1] + this->GlobalOrigin[1];
    pt[2] = ext[4]*spacing[2] + this->GlobalOrigin[2];
    corners[1] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing[0] + this->GlobalOrigin[0];
    pt[1] = (ext[3]+1)*spacing[1] + this->GlobalOrigin[1];
    pt[2] = ext[4]*spacing[2] + this->GlobalOrigin[2];
    corners[2] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing[0] + this->GlobalOrigin[0];
    pt[1] = (ext[3]+1)*spacing[1] + this->GlobalOrigin[1];
    pt[2] = ext[4]*spacing[2] + this->GlobalOrigin[2];
    corners[3] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing[0] + this->GlobalOrigin[0];
    pt[1] = ext[2]*spacing[1] + this->GlobalOrigin[1];
    pt[2] = (ext[5]+1)*spacing[2] + this->GlobalOrigin[2];
    corners[4] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing[0] + this->GlobalOrigin[0];
    pt[1] = ext[2]*spacing[1] + this->GlobalOrigin[1];
    pt[2] = (ext[5]+1)*spacing[2] + this->GlobalOrigin[2];
    corners[5] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing[0] + this->GlobalOrigin[0];
    pt[1] = (ext[3]+1)*spacing[1] + this->GlobalOrigin[1];
    pt[2] = (ext[5]+1)*spacing[2] + this->GlobalOrigin[2];
    corners[6] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing[0] + this->GlobalOrigin[0];
    pt[1] = (ext[3]+1)*spacing[1] + this->GlobalOrigin[1];
    pt[2] = (ext[5]+1)*spacing[2] + this->GlobalOrigin[2];
    corners[7] = pts->InsertNextPoint(pt);
    // Now the faces.
    face[0] = corners[0];
    face[1] = corners[1];
    face[2] = corners[3];
    face[3] = corners[2];
    faces->InsertNextCell(4, face); // -z
    face[0] = corners[4];
    face[1] = corners[6];
    face[2] = corners[7];
    face[3] = corners[5];
    faces->InsertNextCell(4, face); // +z
    face[0] = corners[0];
    face[1] = corners[4];
    face[2] = corners[5];
    face[3] = corners[1];
    faces->InsertNextCell(4, face); //-y
    face[0] = corners[2];
    face[1] = corners[3];
    face[2] = corners[7];
    face[3] = corners[6];
    faces->InsertNextCell(4, face); //+y
    face[0] = corners[0];
    face[1] = corners[2];
    face[2] = corners[6];
    face[3] = corners[4];
    faces->InsertNextCell(4, face); //-x
    face[0] = corners[1];
    face[1] = corners[5];
    face[2] = corners[7];
    face[3] = corners[3];
    faces->InsertNextCell(4, face); //+x
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    }
    
  pd->SetPoints(pts);
  pd->SetPolys(faces);
  levelArray->SetName("Level");
  idArray->SetName("LevelBlockId");
  pd->GetCellData()->AddArray(levelArray);
  pd->GetCellData()->AddArray(idArray);

  vtkXMLPolyDataWriter* w = vtkXMLPolyDataWriter::New();
  w->SetInput(pd);
  w->SetFileName(fileName);
  w->Write();
  
  w->Delete();

  pd->Delete();
  pts->Delete();
  faces->Delete();
  idArray->Delete();
  levelArray->Delete();
}

//----------------------------------------------------------------------------
// Save out the blocks surfaces so we can se what is creating so many ghost cells.
void vtkCTHFragmentConnect::SaveGhostSurfaces(const char* fileName)
{
  vtkPolyData* pd = vtkPolyData::New();
  vtkPoints* pts = vtkPoints::New();
  vtkCellArray* faces = vtkCellArray::New();
  vtkIntArray* idArray = vtkIntArray::New();
  vtkIntArray* levelArray = vtkIntArray::New();
  vtkCTHFragmentConnectBlock* block;
  const int *ext;
  vtkIdType corners[8];
  vtkIdType face[4];
  double pt[3];
  int level;
  int levelId;
  double spacing;
  

  unsigned int ii;
  
  for (ii = 0; ii < this->GhostBlocks.size(); ++ii)
    {
    block = this->GhostBlocks[ii];
    ext = block->GetBaseCellExtent();
    levelId = ii;
    level = block->GetLevel();
    spacing = 1.0 / (double)(1 << level);
    // Insert the points.
    pt[0] = ext[0]*spacing;
    pt[1] = ext[2]*spacing;
    pt[2] = ext[4]*spacing;
    corners[0] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing;
    pt[1] = ext[2]*spacing;
    pt[2] = ext[4]*spacing;
    corners[1] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing;
    pt[1] = (ext[3]+1)*spacing;
    pt[2] = ext[4]*spacing;
    corners[2] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing;
    pt[1] = (ext[3]+1)*spacing;
    pt[2] = ext[4]*spacing;
    corners[3] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing;
    pt[1] = ext[2]*spacing;
    pt[2] = (ext[5]+1)*spacing;
    corners[4] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing;
    pt[1] = ext[2]*spacing;
    pt[2] = (ext[5]+1)*spacing;
    corners[5] = pts->InsertNextPoint(pt);
    pt[0] = ext[0]*spacing;
    pt[1] = (ext[3]+1)*spacing;
    pt[2] = (ext[5]+1)*spacing;
    corners[6] = pts->InsertNextPoint(pt);
    pt[0] = (ext[1]+1)*spacing;
    pt[1] = (ext[3]+1)*spacing;
    pt[2] = (ext[5]+1)*spacing;
    corners[7] = pts->InsertNextPoint(pt);
    // Now the faces.
    face[0] = corners[0];
    face[1] = corners[1];
    face[2] = corners[3];
    face[3] = corners[2];
    faces->InsertNextCell(4, face); // -z
    face[0] = corners[4];
    face[1] = corners[6];
    face[2] = corners[7];
    face[3] = corners[5];
    faces->InsertNextCell(4, face); // +z
    face[0] = corners[0];
    face[1] = corners[4];
    face[2] = corners[5];
    face[3] = corners[1];
    faces->InsertNextCell(4, face); //-y
    face[0] = corners[2];
    face[1] = corners[3];
    face[2] = corners[7];
    face[3] = corners[6];
    faces->InsertNextCell(4, face); //+y
    face[0] = corners[0];
    face[1] = corners[2];
    face[2] = corners[6];
    face[3] = corners[4];
    faces->InsertNextCell(4, face); //-x
    face[0] = corners[1];
    face[1] = corners[5];
    face[2] = corners[7];
    face[3] = corners[3];
    faces->InsertNextCell(4, face); //+x
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    idArray->InsertNextValue(levelId);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    levelArray->InsertNextValue(level);
    }
    
  pd->SetPoints(pts);
  pd->SetPolys(faces);
  levelArray->SetName("Level");
  idArray->SetName("LevelBlockId");
  pd->GetCellData()->AddArray(levelArray);
  pd->GetCellData()->AddArray(idArray);

  vtkXMLPolyDataWriter* w = vtkXMLPolyDataWriter::New();
  w->SetInput(pd);
  w->SetFileName(fileName);
  w->Write();
  
  w->Delete();

  pd->Delete();
  pts->Delete();
  faces->Delete();
  idArray->Delete();
  levelArray->Delete();
}







//
// 1: We have three tasks:  Connect and index face connected cells.
//    Create faces of connected cells.
//    Share points but not all points.  Split points to make topology manifold.
// 2: Connectivity is straight forward depth or breadth first search.
// 3: We can have a special id for not visited and a special id for empty.
// 4: Depth first is easier but we must protect for stack overflow.
// 5: For the surface, we are simply going to take the surface of the voxels
//    and place the verticies based on volume fraction values.
// 6: We could walk the surface in a second pass, but that would be complicated.
// 7: It would be easier to simply create faces as the connectivity search
//    terminates.
// 8: Merging points is tricky.  We want to share points on the surface, 
//    but split points to keep a manifold surface topology.
// 9: We can easily determine which points are new or have been created already
//    when we add a new face.  We simply have to design a way to lookup previously 
//    created points.
// 10: How about preferentially searching voxels with surfaces to minimize 
//    the number of surface points that we need to hash.  This would be complicated
//    and could not eliminate the need to hash points.
// 11: The simple solution is to just use a point locator.
// 12: We could pass in points we know (suspect) will be used by the next cell.
// 13: Why not just have an array of ids as a locator.  We can split as we need to.
//    We could not, however, hash multiple points for a single corner.
// 14: All point separated for first implementation.
// 15: Things I have to ask Jerry:
//      Where should the mass properties be placed?  
//          Separate API in class
//          Field data.
//          Cell or point data.
//      Should I handle multiple volume fractions or just one?  Equivalent (layers)....
// 16: Move corner point along gradient of trilinear interpolation of volume fraction.
// 17: Trying to solve for exact kx, ky, and kz.
// 18: Gradient (-V000-V001-V010-V011+V100+V101+V110+V111,
//               -V000-V001+V010+V011-V100-V101+V110+V111,
//               -V000+V001-V010+V011-V100+V101-V110+V111)
//     Normalize the gradiant vector into (nx,ny,nz)
// 19: Can we solve for k: V((0.5,0.5,0.5)+k(n)) = 0.5; (Cubic equation).
//     It would be a pain.  Lets just assume linear and scale based on magnitude of gradient
//     at the center.  We can at least threshold to keep point inside the cube.
// 20: I can imagine the gradient being 0 at a saddle point. This would cause a sigularity.
//     We could pick a direction, compute the center and a point in the surface (or interior)
//     and
// 21: We also have to deal with points on the boundary.  Lets just duplicate the values
//     to degenerate into bilinear or simply linear   
// 22: First implementation for image data works well!         
// 23: Move corner points in the direction of the gradient (after threshold!) gives
//     the best mesh.
// 24: Todo in the next phase:
//       Share points
//       compute normals
//       compute mass properties
//       save fragment id as cell data
//       convert to AMR input
//       parallelize algorithm
// 25: AMR: How are the scalars stored?  I assume one vtkScalars per block.
//          How do I find neighboring blocks? Loop though and look at extents.
//            Build up a neighbor object that store all extents ...
//          I need a test data set.
//          How do I strip off the ghost cells that I will not need? 

// We have connectivity working with iterators across blocks of different levels.
// We handle level transitions for connectivity and creating faces.
// We place corners using neighbors from different blocks and levels.
//   We look at point neighbors rather than face neighbors.

// 26:  Lets create ghost blocks that have only one layer of cells that adjoin blocks
//      in the process.  There may be a case where two ghost blocks are used to 
//      represent a single block.  They will share/duplicate a row of voxels.
//      We can do connectivity through the ghost cells (might as well) but
//      the do not contribute to the integrated values.  We will not create faces
//      from a ghost layer, but we will use them to place points.
//      We will use the fragment ids in the ghost layers to create an equivalency realtion
//      and to combine fragments in a post processing step. 




// We need to strip off ghost cells that we do not need.
// We need to handle rectilinear grids.
// We need to protect against bad extrapolation spikes.
// There are still cracks because faces are created with at most 4 points.
// We need to add extra points to faces that adjoin higher level faces.
// We still need to protect against running out of stack space.
// We still need to handle multiple processes.
// We need to save out fragment information into file.


// Distributed: Easy.
// Create a ghost layer around each process partition.
// Each process executes connectivity algorithm.
// Connectivity through ghost cells will reduce the number of fragments.
// Create an equivalence set of fragments.
// Compute new fragments and properties.





// Coupled simulations
// Runtime: Tienchu: NDGM HDF5 XDMF
// Compressing data - for faster visualization.
//


//----------------------------------------------------------------------------
// This is for finding equivalent fragment ids within a single process.
// I thought of loops and chains but both have problems.  
// Chains can leave orphans, loops break when two nodes in the loop are 
// equated a second time.
// Lets try a directed tree
void vtkCTHFragmentConnect::AddEquivalence(
  vtkCTHFragmentConnectIterator *neighbor1,
  vtkCTHFragmentConnectIterator *neighbor2)
{
  int id1 = *(neighbor1->FragmentIdPointer);
  int id2 = *(neighbor2->FragmentIdPointer);

  if (id1 != id2 && id1 != -1 && id2 != -1)
    {
    this->EquivalenceSet->AddEquivalence(id1, id2);
    }
}

//----------------------------------------------------------------------------
// When we enter this method the equivalent set has bee resolved so
// it is indexed by global raw (generated by connectivity) fragment ids and the
// values are a sequential final global fragment ids.
// The volume array is indexed by local raw fragment indexes.
// When this method finishes, all processes have a volume array indexed
// by the final global fragment ids and the values are the volumes.
void vtkCTHFragmentConnect::ResolveVolumes()
{
  int myProc = this->Controller->GetLocalProcessId();
  int numProcs = this->Controller->GetNumberOfProcesses();
  double *ptr;
  int numRawFragments;

  // Allocate the new array to receive the resolved global volumes.
  vtkDoubleArray* resolvedVolumes = vtkDoubleArray::New();
  resolvedVolumes = vtkDoubleArray::New();
  resolvedVolumes->SetNumberOfTuples(this->NumberOfResolvedFragments);

  // First: Send all the raw volumes to process 0 to be merged.
  if (myProc > 0)
    {
    numRawFragments = this->NumberOfRawFragmentsInProcess[myProc];
    ptr = this->FragmentVolumes->GetPointer(0);
    this->Controller->Send(ptr, numRawFragments, 0, 222277);
    // Now receive the resolved volumes.
    ptr = resolvedVolumes->GetPointer(0);
    this->Controller->Receive(ptr, this->NumberOfResolvedFragments, 0, 222278);
    this->FragmentVolumes->Delete();
    this->FragmentVolumes = resolvedVolumes;
    return;
    }
  
  // This is process 0.  We need to receive the arrays and resolve them.
  double* globalRawFragmentVolumes = new double[this->TotalNumberOfRawFragments];
  // Copy the local fragments into the global array.
  numRawFragments = this->NumberOfRawFragmentsInProcess[0];
  ptr = this->FragmentVolumes->GetPointer(0);
  memcpy(globalRawFragmentVolumes, ptr, numRawFragments*sizeof(double));
  for (int ii = 1; ii < numProcs; ++ii)
    {
    numRawFragments = this->NumberOfRawFragmentsInProcess[ii];
    this->Controller->Receive(globalRawFragmentVolumes+this->LocalToGlobalOffsets[ii],
                              numRawFragments, ii, 222277);
    }
  // Now sum up the volume from equivalent fragments.
  ptr = resolvedVolumes->GetPointer(0);
  for (int ii = 0; ii < this->NumberOfResolvedFragments; ++ii)
    {
    ptr[ii] = 0.0;
    }
  for (int ii = 0; ii < this->TotalNumberOfRawFragments; ++ii)
    {
    int finalId = this->EquivalenceSet->GetEquivalentSetId(ii);
    ptr[finalId] += globalRawFragmentVolumes[ii];
    }
  for (int ii = 1; ii < numProcs; ++ii)
    {
    this->Controller->Send(ptr, this->NumberOfResolvedFragments, ii, 222278);
    }
  this->FragmentVolumes->Delete();
  this->FragmentVolumes = resolvedVolumes;

  delete [] globalRawFragmentVolumes;
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::ResolveEquivalences(vtkIntArray* fragmentIdArray)
{
  int myProc = this->Controller->GetLocalProcessId();
  int numProcs = this->Controller->GetNumberOfProcesses();
  // These get resused for efer attribute we need to merge so I made the ivars.
  this->NumberOfRawFragmentsInProcess = new int[numProcs];
  this->LocalToGlobalOffsets = new int[numProcs];

  // Go through the equivalence array collapsing chains 
  // and assigning consecutive ids.

  // Resolve intraprocess and extra process equivalences.
  // This also renumbers set ids to be sequential.
  this->GatherEquivalenceSets(this->EquivalenceSet);
  int localToGlobal = this->LocalToGlobalOffsets[myProc];
  
  // Now change the ids in the cell / point array.
  int num = fragmentIdArray->GetNumberOfTuples();
  for (int ii = 0; ii < num; ++ii)
    {
    int id = fragmentIdArray->GetValue(ii);
    if (id < 0)
      { // Sanity check.
      vtkErrorMacro("Id out of range.");
      }
    else
      {
      int newId = this->EquivalenceSet->GetEquivalentSetId(id+localToGlobal);
      fragmentIdArray->SetValue(ii,newId);
      }
    }
  
  // Use the resolved ids to resolve volumes.
  this->ResolveVolumes();
  
  delete [] this->NumberOfRawFragmentsInProcess;
  this->NumberOfRawFragmentsInProcess = 0;
  delete [] this->LocalToGlobalOffsets;
  this->LocalToGlobalOffsets = 0;
}


//----------------------------------------------------------------------------
// This also fills in the arrays NumberOfRawFragments and LocalToGlobalOffsets
// as a side effect. (also NumberOfResolvedFragments).
void vtkCTHFragmentConnect::GatherEquivalenceSets(
  vtkCTHFragmentEquivalenceSet* set)
{
  int numProcs = this->Controller->GetNumberOfProcesses();
  int myProcId = this->Controller->GetLocalProcessId();
  int numLocalMembers = set->GetNumberOfMembers();

  // Find a mapping between local fragment id and the global fragment ids.
  if (myProcId == 0)
    {
    this->NumberOfRawFragmentsInProcess[0] = numLocalMembers;
    for (int ii = 1; ii < numProcs; ++ii)
      {
      this->Controller->Receive(this->NumberOfRawFragmentsInProcess+ii, 1, ii, 875034);
      }
    // Now send the results back to all processes.
    for (int ii = 1; ii < numProcs; ++ii)
      {
      this->Controller->Send(this->NumberOfRawFragmentsInProcess, numProcs, ii, 875035);
      }
    }
  else
    {
    this->Controller->Send(&numLocalMembers, 1, 0, 875034);
    this->Controller->Receive(this->NumberOfRawFragmentsInProcess, numProcs, 0, 875035);
    }
  // Compute offsets.
  int totalNumberOfIds = 0;
  for (int ii = 0; ii < numProcs; ++ii)
    {
    int numIds = this->NumberOfRawFragmentsInProcess[ii];
    this->LocalToGlobalOffsets[ii] = totalNumberOfIds;
    totalNumberOfIds += numIds;
    }
  this->TotalNumberOfRawFragments = totalNumberOfIds;

  // Change the set to a global set.
  vtkCTHFragmentEquivalenceSet *globalSet = new vtkCTHFragmentEquivalenceSet;
  // This just initializes the set so that every set has one member.
  //  Every id is equivalent to itself and no others.
  if (totalNumberOfIds > 0)
    { // This crashes if there no fragemnts extracted.
    globalSet->AddEquivalence(totalNumberOfIds-1, totalNumberOfIds-1);
    }
  // Add the equivalences from our process.
  int myOffset = this->LocalToGlobalOffsets[myProcId];
  int memberSetId;
  for(int ii = 0; ii < numLocalMembers; ++ii)
    {
    memberSetId = set->GetEquivalentSetId(ii);
    // This will be inefficient until I make the scheduled improvemnt to the
    // add method (Ordered multilevel tree instead of a one level tree).
    globalSet->AddEquivalence(ii+myOffset, memberSetId+myOffset);
    }

  //cerr << myProcId << " Input set: " << endl;
  //set->Print();
  //cerr << myProcId << " global set: " << endl;
  //globalSet->Print();

  // Now add equivalents between processes.
  // Send all the ghost blocks to the process that owns the block.
  // Compare ids and add the equivalences.
  this->ShareGhostEquivalences(globalSet, this->LocalToGlobalOffsets);

  //cerr << "Global after ghost: " << myProcId << endl;
  //globalSet->Print();

  // Merge all of the processes global sets.
  // Clean the global set so that the resulting set ids are sequential.
  this->MergeGhostEquivalenceSets(globalSet);

  //cerr << "Global after merge: " << myProcId << endl;
  //globalSet->Print();

  // Copy the equivalences to the local set for returning our results.
  // The ids will be the global ids so the GetId method will work.
  set->DeepCopy(globalSet);
}

//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::MergeGhostEquivalenceSets(
  vtkCTHFragmentEquivalenceSet* globalSet)
{
  int myProcId = this->Controller->GetLocalProcessId();
  int* buf = globalSet->GetPointer();
  int numIds = globalSet->GetNumberOfMembers();
  
  // At this point all the sets are global and have the same number of ids.
  // Send all the sets to process 0 to be merged.
  if (myProcId > 0)
    {
    this->Controller->Send(buf, numIds, 0, 342320);
    // Now receive the final equivalences.
    this->Controller->Receive(&this->NumberOfResolvedFragments, 1, 0, 342321);
    // Domain has numIds,  range has NumberOfResolvedFragments
    this->Controller->Receive(buf, numIds, 0, 342322);
    // We have to mark the set as resolved because the set being
    // received has been resolved.  If we do not do this then
    // We cannot get the proper set id.  Using the pointer
    // here is a bad api.  TODO: Fix the API and make "Resolved" private.
    globalSet->Resolved = 1;
    return;
    }

  // Only process 0 from here out.

  int numProcs = this->Controller->GetNumberOfProcesses();
  int* tmp;
  tmp = new int[numIds];
  for (int ii = 1; ii < numProcs; ++ii)
    {
    this->Controller->Receive(tmp, numIds, ii, 342320);
    // Merge the values.
    for (int jj = 0; jj < numIds; ++jj)
      {
      if (tmp[jj] != jj)
        { // TODO: Make sure this is efficient.  Avoid n^2.
        globalSet->AddEquivalence(jj, tmp[jj]);
        }
      }
    }

  // Make the set ids sequential.
  this->NumberOfResolvedFragments = globalSet->ResolveEquivalences();
    
  // The pointers should still be valid.
  // The array should not resize here.
  // Number of resolved fragemnts will be smaller 
  // than TotalNumberOfRawFragments
  for (int ii = 1; ii < numProcs; ++ii)
    {
    this->Controller->Send(&this->NumberOfResolvedFragments, 1, ii, 342321);
    // Domain has numIds,  range has NumberOfResolvedFragments
    this->Controller->Send(buf, numIds, ii, 342322);
    }
 }

//----------------------------------------------------------------------------
void vtkCTHFragmentConnect::ShareGhostEquivalences(
  vtkCTHFragmentEquivalenceSet* globalSet,
  int* procOffsets)
{
  int numProcs = this->Controller->GetNumberOfProcesses();
  int myProcId = this->Controller->GetLocalProcessId();
  int sendMsg[8];

  // Loop through the other processes.
  for (int otherProc = 0; otherProc < numProcs; ++otherProc)
    {
    if (otherProc == myProcId)
      {
      this->ReceiveGhostFragmentIds(globalSet, procOffsets);
      }
    else
      {
      // Loop through our ghost blocks sending the 
      // ones that are owned by otherProc.
      int num = this->GhostBlocks.size();
      for (int blockId = 0; blockId < num; ++blockId)
        {
        vtkCTHFragmentConnectBlock* block = this->GhostBlocks[blockId];
        if (block && block->GetOwnerProcessId() == otherProc && block->GetGhostFlag())
          {
          sendMsg[0] = myProcId;
          // Since this is a ghost block, the remote block id
          // will be different than the id we use.
          // We just want to make it easy for the process that owns this block
          // to match the ghost block with the aoriginal.
          sendMsg[1] = block->GetBlockId();
          int* ext = sendMsg + 2;
          block->GetCellExtent(ext);
          this->Controller->Send(sendMsg, 8, otherProc, 722265);
          // Now send the fragment id array.
          int* framentIds = block->GetFragmentIdPointer();
          this->Controller->Send(framentIds, (ext[1]-ext[0]+1)*(ext[3]-ext[2]+1)*(ext[5]-ext[4]+1),
                                 otherProc, 722266);
          } // End if ghost  block owned by other process.
        } // End loop over all blocks.
      // Send the message that indicates we have nothing more to send.
      sendMsg[0] = myProcId;
      sendMsg[1] = -1;
      this->Controller->Send(sendMsg, 8, otherProc, 722265);
      } // End if we shoud send or receive.
    } // End loop over all processes.
}

//----------------------------------------------------------------------------
// Receive all the gost blocks from remote processes and 
// find the equivalences.
void vtkCTHFragmentConnect::ReceiveGhostFragmentIds(
  vtkCTHFragmentEquivalenceSet* globalSet,
  int* procOffsets)
{
  int msg[8];
  int otherProc;
  int blockId;
  vtkCTHFragmentConnectBlock* block;
  int bufSize = 0;
  int* buf = 0;
  int dataSize;
  int* remoteExt;
  int localId, remoteId;
  int myProcId = this->Controller->GetLocalProcessId();
  int localOffset = procOffsets[myProcId];
  int remoteOffset;

  // We do not receive requests from our own process.
  int remainingProcs = this->Controller->GetNumberOfProcesses() - 1;
  while (remainingProcs != 0)
    {
    this->Controller->Receive(msg, 8, vtkMultiProcessController::ANY_SOURCE, 722265);
    otherProc = msg[0];
    blockId = msg[1];
    if (blockId == -1)
      {
      --remainingProcs;
      }
    else
      {
      // Find the block.
      block = this->InputBlocks[blockId];
      if (block == 0)
        { // Sanity check. This will lock up!
        vtkErrorMacro("Missing block request.");
        return;
        }
      // Receive the ghost fragment ids.
      remoteExt = msg+2;
      dataSize = (remoteExt[1]-remoteExt[0]+1)
                  *(remoteExt[3]-remoteExt[2]+1)
                  *(remoteExt[5]-remoteExt[4]+1);
      if (bufSize < dataSize) 
        {
        if (buf) { delete [] buf;}
        buf = new int[dataSize];
        bufSize = dataSize;
        }
      remoteOffset = procOffsets[otherProc];
      this->Controller->Receive(buf, dataSize, otherProc, 722266);
      // We have our block, and the remote fragmentIds.
      // Now for the equivalences.
      // Loop through all of the voxels.
      int* remoteFragmentIds = buf;
      int* localFragmentIds = block->GetFragmentIdPointer();
      int localExt[6];
      int localIncs[3];
      block->GetCellExtent(localExt);
      block->GetCellIncrements(localIncs);
      int *px, *py, *pz;
      // Find the starting voxel in the local block.
      pz = localFragmentIds + (remoteExt[0]-localExt[0])*localIncs[0]
                            + (remoteExt[2]-localExt[2])*localIncs[1]
                            + (remoteExt[4]-localExt[4])*localIncs[2];
      for (int iz = remoteExt[4]; iz <= remoteExt[5]; ++iz)
        {
        py = pz;
        for (int iy = remoteExt[2]; iy <= remoteExt[3]; ++iy)
          {
          px = py;
          for (int ix = remoteExt[0]; ix <= remoteExt[1]; ++ix)
            {
            // Convert local fragment ids to global ids.
            localId = *px;
            remoteId = *remoteFragmentIds;
            if (localId >= 0 && remoteId >= 0)
              {
              globalSet->AddEquivalence(localId + localOffset, 
                                        remoteId + remoteOffset);
              }
            ++remoteFragmentIds;
            ++px;
            }
          py += localIncs[1];
          }
        pz += localIncs[2];
        }
      }
    }
  if (buf)
    {
    delete [] buf;
    }
}


//----------------------------------------------------------------------------
// Make a cell array that has fragment volume as a temporary solution.
// The volume array should really be field data.
void vtkCTHFragmentConnect::GenerateVolumeArray(
  vtkIntArray* fragmentIds,
  vtkPolyData *output)
{
  int numPoints = fragmentIds->GetNumberOfTuples();
  int* idPtr = fragmentIds->GetPointer(0);
  
  vtkDoubleArray* va = vtkDoubleArray::New();
  va->SetName("FragmentVolume");
  va->SetNumberOfTuples(numPoints);
  
  for (int ii = 0; ii < numPoints; ++ii)
    {
    va->InsertTuple1(ii, this->FragmentVolumes->GetTuple1(*idPtr++));
    }

  output->GetPointData()->AddArray(va);
  va->Delete();
}


/*
//----------------------------------------------------------------------------
// We could integrate the attributes in a second pass.  It would not be too
// expensive because we would not have to worry about neighbors between blocks
// or processes.  Just a simple traversal of the fragment id, volume fraction 
// and attibutes (block by block).  The only advantage I can see for this is
// the knowlege of how many framents there would be would keep the 
// fragment integration arrays from reallocating.
// I am just going to integrate the attributes as the voxels are visited for 
// connectivity
void vtkCTHFragmentConnect::InitializeAttributeIntegration(vtkCellData* inCellData)
{
  int numComps;
  int bufferLength = 0;

  // This is where we store the attributes for each fragment
  // after integration.  This is not the final celldata array
  // because equivalent framgents are merge.
  this->IntegratedFragmentAttributes = vtkCellData::New();
  
  // Allocate arrays.
  int numArrays = inCellData->GetNumberOfArrays();
  for (int ii = 0; ii < numArrays; ++ii)
    {
    vtkDataArray* inArray = inCellData->GetArray(ii);
    // Make a new array of the same type.
    vtkDataArray* fragArray = inArray->NewInstance();
    numComps = inArray->GetNumberOfComponents();
    fragArray->SetNumberOfComponents(numComps);
    fragArray->SetName(inArray->GetName());
    this->IntegratedFragmentAttributes->AddArray(fragArray);
    bufLength += numComps * fragArray->GetDataTypeSize();
    }
  
  // I am going to do the actual integration in a raw memory buffer.
  // It is flexible with no complicated arbitrary structure.
  // I just iterate through the buffer casting the pointer to the correct types.
  this->IntegrationBuffer = new unsigned char[bufferLength];
}

//----------------------------------------------------------------------------
// This is called every time we visit a voxel that is part of a fragment.
void vtkCTHFragmentConnect::IntegrateFragmentAttributesForVoxel(
  vtkIdType voxelId, 
  double volumeFraction,
  vtkCellData* inCellData)
{
  void* integrationPtr = this->IntegrationBuffer();
  
  int numArrays = inCellData->GetNumberOfArrays();
  for (int ii = 0; ii < numArrays; ++ii)
    {
    vtkDataArray* inArray = inCellData->GetArray(ii);
    numComps = inArray->GetNumberOfComponents();
    // What should we do if the array is an int or byte ???
    // We should probably skip the volume fractions.????????
    switch (inArray->GetDataType())
      {
      case VTK_FLOAT :
        // We could template this ...
        float* inPtr = ((vtkFloatArray*)(inArray))->GetPointer(0);
        float* floatIntegrationPtr = ((float*)(integrationPtr));
        for (int comp = 0; comp < numComps; ++comp)
          {
          *floatIntegrationPtr++ += *inPtr++;
          }
        integrationPtr = (void*)(floatIntegrationPtr);
        break;
      case VTK_DOUBLE :
        float* inPtr = ((vtkFloatArray*)(inArray))->GetPointer(0);
        float* floatIntegrationPtr = ((float*)(integrationPtr));
        for (int comp = 0; comp < numComps; ++comp)
          {
          *floatIntegrationPtr++ += *inPtr++;
          }
        integrationPtr = (void*)(floatIntegrationPtr);
        break;
    }
}

//----------------------------------------------------------------------------
// This should be called when the connectivity moves to a new framment.
void vtkCTHFragmentConnect::FinishFragmentAttributeIntgration()
{
}

//----------------------------------------------------------------------------
// This resolves all equivalent fragment, places the new arrays in the
// output cell data, and frees all memory used for the integration.
void vtkCTHFragmentConnect::FinializeAttributeIntegration(..)

*/


