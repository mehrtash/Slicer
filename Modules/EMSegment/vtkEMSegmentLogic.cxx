#include <string>
#include <iostream>
#include <sstream>
#include <algorithm>

#include "vtkObjectFactory.h"
#include "vtkImageChangeInformation.h"

#include "vtkEMSegmentLogic.h"
#include "vtkEMSegment.h"

#include "vtkMRMLScene.h"

#include "vtkMRMLEMSNode.h"
#include "vtkMRMLEMSSegmenterNode.h"
#include "vtkMRMLEMSTemplateNode.h"
#include "vtkMRMLEMSTreeNode.h"
#include "vtkMRMLEMSTreeParametersLeafNode.h"
#include "vtkMRMLEMSTreeParametersParentNode.h"
#include "vtkMRMLEMSTreeParametersNode.h"
#include "vtkMRMLEMSWorkingDataNode.h"
#include "vtkMRMLEMSIntensityNormalizationParametersNode.h"
#include "vtkImageEMLocalSegmenter.h"
#include "vtkImageEMLocalSuperClass.h"
#include "vtkImageMeanIntensityNormalization.h"
#include "vtkMath.h"
#include "vtkImageReslice.h"
#include "vtkRigidRegistrator.h"
#include "vtkBSplineRegistrator.h"
#include "vtkTransformToGrid.h"
#include "vtkIdentityTransform.h"

// needed to translate between enums
#include "EMLocalInterface.h"

#include <math.h>
#include <exception>

#define ERROR_NODE_VTKID 0

//----------------------------------------------------------------------------
vtkEMSegmentLogic* vtkEMSegmentLogic::New()
{
  // First try to create the object from the vtkObjectFactory
  vtkObject* ret = 
    vtkObjectFactory::CreateInstance("vtkEMSegmentLogic");
  if(ret)
    {
    return (vtkEMSegmentLogic*)ret;
    }
  // If the factory was unable to create the object, then create it here.
  return new vtkEMSegmentLogic;
}


//----------------------------------------------------------------------------
vtkEMSegmentLogic::vtkEMSegmentLogic()
{
  this->ModuleName = NULL;

  this->ProgressCurrentAction = NULL;
  this->ProgressGlobalFractionCompleted = 0.0;
  this->ProgressCurrentFractionCompleted = 0.0;

  //this->DebugOn();

  this->MRMLManager = NULL; // NB: must be set before SetMRMLManager is called
  vtkEMSegmentMRMLManager* manager = vtkEMSegmentMRMLManager::New();
  this->SetMRMLManager(manager);
  manager->Delete();
}

//----------------------------------------------------------------------------
vtkEMSegmentLogic::~vtkEMSegmentLogic()
{
  this->SetMRMLManager(NULL);
  this->SetProgressCurrentAction(NULL);
  this->SetModuleName(NULL);
}

//----------------------------------------------------------------------------
void vtkEMSegmentLogic::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
  // !!! todo
}

//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
SaveTemplateNow()
{
  //
  // for now just save entire mrml scene, later it might be helpful to
  // decide which nodes are important and which are unimportant
  //
  
  // according to comments this should be "file://path/file.xml"
  // but that does not work

  if (this->MRMLManager->GetNode() == NULL)
    {
    vtkErrorMacro("Attempting to save scene but EMSNode is null");
    return;
    }
  vtksys_stl::string url = ""; 
  url += this->MRMLManager->GetSaveTemplateFilename();
  this->GetMRMLScene()->Commit(url.c_str());
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
SaveIntermediateResults()
{
  //
  // get output directory
  std::string outputDirectory(this->MRMLManager->GetSaveWorkingDirectory());

  if (!vtksys::SystemTools::FileExists(outputDirectory.c_str()))
    {
    // directory does not exist, bail out (and let the user create it!)
    return false;
    }  

  //
  // package EMSeg-related parameters together and write them to disk
  bool writeSuccessful = 
    this->MRMLManager->PackageAndWriteData(outputDirectory.c_str());

  return writeSuccessful;
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
StartPreprocessing()
{
  if (!this->MRMLManager->GetWorkingDataNode())
  {
    vtkErrorMacro("Can't preprocess because WorkingDataNode is null.");    
    return false;
  }

  if (!this->StartPreprocessingInitializeInputData())
    {
    vtkWarningMacro
      ("EMSEG Preprocessing Error: Failed to initialize input data");
    return false;
    }
  if (!this->StartPreprocessingTargetIntensityNormalization())
    {
    vtkWarningMacro
      ("EMSEG Preprocessing Error: Failed to normalize target images");
    return false;
    }
  if (!this->StartPreprocessingTargetToTargetRegistration())
    {
    vtkWarningMacro
      ("EMSEG Preprocessing Error: Failed to register target images");
    return false;
    }
  if (!this->StartPreprocessingAtlasToTargetRegistration())
    {
    vtkWarningMacro
      ("EMSEG Preprocessing Error: Failed to register atlas images");
    return false;
    }
  // all OK
  return true;
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
StartPreprocessingInitializeInputData()
{
  // set the input to the working data
  // when do we return false? !!!
  this->MRMLManager->GetWorkingDataNode()->SetInputTargetNodeID
    (this->MRMLManager->GetSegmenterNode()->GetTargetNodeID());
  this->MRMLManager->GetWorkingDataNode()->SetInputAtlasNodeID
    (this->MRMLManager->GetSegmenterNode()->GetAtlasNodeID());
  return true;
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
StartPreprocessingTargetIntensityNormalization()
{
  std::cerr << " EMSEG: Starting intensity normalization..." << std::endl;

  // get a pointer to the mrml manager for easy access
  vtkEMSegmentMRMLManager* m = this->MRMLManager;

  // get input target from working node
  vtkMRMLEMSTargetNode* inputTarget = 
    m->GetWorkingDataNode()->GetInputTargetNode();
  if (inputTarget == NULL)
    {
    vtkWarningMacro("Input target node is null, aborting!");
    return false;
    }
  
  // check that global parameters exist
  if (!this->MRMLManager->GetGlobalParametersNode())
    {
    vtkWarningMacro("Global parameters node is null, aborting!");
    return false;
    }
  
  // set up the normalized target node
  vtkMRMLEMSTargetNode* normalizedTarget = 
    m->GetWorkingDataNode()->GetNormalizedTargetNode();
  if (!normalizedTarget)
    {
    // clone intput to new normalized target node
    std::cerr << "  Cloning target node...";
    normalizedTarget = m->CloneTargetNode(inputTarget, "NormalizedTarget");
    std::cerr << "Number of images is: " 
              << normalizedTarget->GetNumberOfVolumes() << "..." << std::endl;
    m->GetWorkingDataNode()->
      SetNormalizedTargetNodeID(normalizedTarget->GetID());
    std::cerr << "Done" << std::endl;
    }
  else
    {
    if (!m->GetUpdateIntermediateData())
      {
      std::cerr << "  Using current normalized images." << std::endl;
      return true;
      }
    }
  
  //
  // check that the number of target images did not change
  // !!! todo !!!

  //
  // apply normalization
  for (int i = 0; i < normalizedTarget->GetNumberOfVolumes(); ++i)
    {
    if (!m->GetNthTargetVolumeIntensityNormalizationEnabled(i))
      {
      // don't apply normaliation to this image
      std::cerr << "  Skipping image " << i 
                << " (no normalization requested)." << std::endl;
      continue;
      }
    std::cerr << "  Normalizing image " << i << "..." << std::endl;
    
    // get image data
    vtkImageData* inData = 
      inputTarget->GetNthVolumeNode(i)->GetImageData();
    vtkImageData* outData = 
      normalizedTarget->GetNthVolumeNode(i)->GetImageData(); 
    if (inData == NULL)
      {
      vtkErrorMacro("Normalization input is null, skipping: " << i);
      continue;
      }
    if (outData == NULL)
      {
      vtkErrorMacro("Normalization output is null, skipping: " << i);
      continue;
      }

    // setup vtk filter
    vtkImageMeanIntensityNormalization* normFilter =
      vtkImageMeanIntensityNormalization::New();
    normFilter->SetNormValue
      (m->GetNthTargetVolumeIntensityNormalizationNormValue(i));
    normFilter->SetNormType
      (m->GetNthTargetVolumeIntensityNormalizationNormType(i));
    normFilter->SetInitialHistogramSmoothingWidth
      (m->
       GetNthTargetVolumeIntensityNormalizationInitialHistogramSmoothingWidth(i));
    normFilter->SetMaxHistogramSmoothingWidth
      (m->GetNthTargetVolumeIntensityNormalizationMaxHistogramSmoothingWidth(i));
    normFilter->SetRelativeMaxVoxelNum
      (m->GetNthTargetVolumeIntensityNormalizationRelativeMaxVoxelNum(i));
    normFilter->SetPrintInfo
      (m->GetNthTargetVolumeIntensityNormalizationPrintInfo(i));
    normFilter->SetInput(inData);

    // execute filter
    try
      {
      normFilter->Update();
      }
    catch (...)
      {
      vtkWarningMacro("Error executing normalization filter for target image " 
                      << i << ".  Skipping this image.");
      }
    outData->ShallowCopy(normFilter->GetOutput());
    normFilter->Delete();
    }
    
  std::cerr << " EMSEG: Normalization complete." << std::endl;

  // intensity statistics, if computed from data, must be updated
  m->UpdateIntensityDistributions();

  return true;
}

void
vtkEMSegmentLogic::
PrintImageInfo(vtkMRMLVolumeNode* volumeNode)
{
  if (volumeNode == NULL || volumeNode->GetImageData() == NULL)
    {
    return;
    }

  // extent
  int extent[6];
  volumeNode->GetImageData()->GetExtent(extent);
  std::cerr << "Extent: " << std::endl;
  std::copy(extent, extent+6, std::ostream_iterator<int>(std::cerr, " "));
  std::cerr << std::endl;
  
  // ijkToRAS
  vtkMatrix4x4* matrix = vtkMatrix4x4::New();
  volumeNode->GetIJKToRASMatrix(matrix);
  std::cerr << "IJKtoRAS Matrix: " << std::endl;
  for (unsigned int r = 0; r < 4; ++r)
    {
    std::cerr << "   ";
    for (unsigned int c = 0; c < 4; ++c)
      {
      std::cerr 
        << matrix->GetElement(r,c)
        << "   ";
      }
    std::cerr << std::endl;
    }  
  matrix->Delete();
}

// a utility to print out a vtk image origin, spacing, and extent
//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
PrintImageInfo(vtkImageData* image)
{
  double spacing[3];
  double origin[3];
  int extent[6];

  image->GetSpacing(spacing);
  image->GetOrigin(origin);
  image->GetExtent(extent);

  std::cerr << "Spacing: ";
  std::copy(spacing,spacing+3,std::ostream_iterator<double>(std::cerr," "));
  std::cerr << std::endl;
  std::cerr << "Origin: ";
  std::copy(origin,origin+3,std::ostream_iterator<double>(std::cerr," "));
  std::cerr << std::endl;
  std::cerr << "Extent: ";
  std::copy(extent,extent+6,std::ostream_iterator<double>(std::cerr," "));
  std::cerr << std::endl;
}

bool 
vtkEMSegmentLogic::
IsVolumeGeometryEqual(vtkMRMLVolumeNode* lhs,
                      vtkMRMLVolumeNode* rhs)
{
  if (lhs == NULL || rhs == NULL ||
      lhs->GetImageData() == NULL || rhs->GetImageData() == NULL)
    {
    return false;
    }

  // check extent
  int extentLHS[6];
  lhs->GetImageData()->GetExtent(extentLHS);
  int extentRHS[6];
  rhs->GetImageData()->GetExtent(extentRHS);
  bool equalExent = std::equal(extentLHS, extentLHS+6, extentRHS);
  
  // check ijkToRAS
  vtkMatrix4x4* matrixLHS = vtkMatrix4x4::New();
  lhs->GetIJKToRASMatrix(matrixLHS);
  vtkMatrix4x4* matrixRHS = vtkMatrix4x4::New();
  rhs->GetIJKToRASMatrix(matrixLHS);  
  bool equalMatrix = std::equal((*matrixLHS)[0], (*matrixLHS)[0]+16, 
                                (*matrixRHS)[0]);

  matrixLHS->Delete();
  matrixRHS->Delete();

  return equalExent && equalMatrix;
}

template <class T>
bool 
vtkEMSegmentLogic::
map_value_comparer(typename std::map<T, unsigned int>::value_type &i1, 
                   typename std::map<T, unsigned int>::value_type &i2)
{
  return i1.second<i2.second;
}

template <class T>
T
vtkEMSegmentLogic::
GuessRegistrationBackgroundLevel(vtkImageData* imageData)
{
  int borderWidth = 5;
  T inLevel;
  typedef std::map<T, unsigned int> MapType;
  MapType m;
  long totalVoxelsCounted = 0;

  T* inData = static_cast<T*>(imageData->GetScalarPointer());
  int dim[3];
  imageData->GetDimensions(dim);

  vtkIdType inc[3];
  vtkIdType iInc, jInc, kInc;
  imageData->GetIncrements(inc);

   // k first slice
  for (int k = 0; k < borderWidth; ++k)
    {
    kInc = k*inc[2];
    for (int j = 0; j < dim[1]; ++j)
      {
      jInc = j*inc[1];
      for (int i = 0; i < dim[0]; ++i)
        {
        iInc = i*inc[0];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }

  // k last slice
  for (int k = dim[2]-borderWidth; k < dim[2]; ++k)
    {
    kInc = k*inc[2];
    for (int j = 0; j < dim[1]; ++j)
      {
      jInc = j*inc[1];
      for (int i = 0; i < dim[0]; ++i)
        {
        iInc = i*inc[0];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }

  // j first slice
  for (int j = 0; j < borderWidth; ++j)
    {
    jInc = j*inc[1];
    for (int k = 0; k < dim[2]; ++k)
      {
      kInc = k*inc[2];
      for (int i = 0; i < dim[0]; ++i)
        {
        iInc = i*inc[0];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }

  // j last slice
  for (int j = dim[1]-borderWidth; j < dim[1]; ++j)
    {
    jInc = j*inc[1];
    for (int k = 0; k < dim[2]; ++k)
      {
      kInc = k*inc[2];
      for (int i = 0; i < dim[0]; ++i)
        {
        iInc = i*inc[0];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }

  // i first slice
  for (int i = 0; i < borderWidth; ++i)
    {
    iInc = i*inc[0];
    for (int k = 0; k < dim[2]; ++k)
      {
      kInc = k*inc[2];
      for (int j = 0; j < dim[1]; ++j)
        {
        jInc = j*inc[1];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }

  // i last slice
  for (int i = dim[0]-borderWidth; i < dim[0]; ++i)
    {
    iInc = i*inc[0];
    for (int k = 0; k < dim[2]; ++k)
      {
      kInc = k*inc[2];
      for (int j = 0; j < dim[1]; ++j)
        {
        jInc = j*inc[1];
        inLevel = inData[iInc+jInc+kInc];
        if (m.count(inLevel))
          {
          ++m[inLevel];
          }
        else
          {
          m[inLevel] = 1;
          }
        ++totalVoxelsCounted;
        }
      }
    }
  
  if (m.empty())
    {
    return 0;
    }
  else
    {
    typename MapType::iterator itor = 
      std::max_element(m.begin(), m.end(),
                       vtkEMSegmentLogic::map_value_comparer<T>);

    T backgroundLevel = itor->first;
    double percentageOfVoxels = 
      100.0 * static_cast<double>(itor->second)/totalVoxelsCounted;
    m.erase(itor);

    typename MapType::iterator itor2 = 
      std::max_element(m.begin(), m.end(),
                       vtkEMSegmentLogic::map_value_comparer<T>);

    std::cerr << "   Background level guess : " 
              << backgroundLevel << "(" << percentageOfVoxels << "%) "
              << "second place: "
              << itor2->first << "(" 
              << 100.0 * static_cast<double>(itor2->second)/totalVoxelsCounted
              << "%)"
              << std::endl;
    
    return backgroundLevel;
    }
}

//
// A Slicer3 wrapper around vtkImageReslice.  Reslice the image data
// from inputVolumeNode into outputVolumeNode with the output image
// geometry specified by outputVolumeGeometryNode.  Optionally specify
// a transform.  The reslice transorm will be:
//
// outputIJK->outputRAS->(outputRASToInputRASTransform)->inputRAS->inputIJK
//
//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
SlicerImageReslice(vtkMRMLVolumeNode* inputVolumeNode,
                   vtkMRMLVolumeNode* outputVolumeNode,
                   vtkMRMLVolumeNode* outputVolumeGeometryNode,
                   vtkTransform* outputRASToInputRASTransform,
                   int interpolationType,
                   double backgroundLevel)
{
  vtkImageData* inputImageData  = inputVolumeNode->GetImageData();
  vtkImageData* outputImageData = outputVolumeNode->GetImageData();
  vtkImageData* outputGeometryData = NULL;
  if (outputVolumeGeometryNode != NULL)
    {
    outputGeometryData = outputVolumeGeometryNode->GetImageData();
    }

  vtkImageReslice* resliceFilter = vtkImageReslice::New();

  //
  // set inputs
  resliceFilter->SetInput(inputImageData);

  //
  // set geometry
  if (outputGeometryData != NULL)
    {
    resliceFilter->SetInformationInput(outputGeometryData);
    outputVolumeNode->CopyOrientation(outputVolumeGeometryNode);
    }

  //
  // setup total transform
  // ijk of output -> RAS -> XFORM -> RAS -> ijk of input
  vtkTransform* totalTransform = vtkTransform::New();
  if (outputRASToInputRASTransform != NULL)
    {
    totalTransform->DeepCopy(outputRASToInputRASTransform);
    }

  vtkMatrix4x4* outputIJKToRAS  = vtkMatrix4x4::New();
  outputVolumeNode->GetIJKToRASMatrix(outputIJKToRAS);
  vtkMatrix4x4* inputRASToIJK = vtkMatrix4x4::New();
  inputVolumeNode->GetRASToIJKMatrix(inputRASToIJK);

  totalTransform->PreMultiply();
  totalTransform->Concatenate(outputIJKToRAS);
  totalTransform->PostMultiply();
  totalTransform->Concatenate(inputRASToIJK);
  resliceFilter->SetResliceTransform(totalTransform);

  //
  // resample the image
  resliceFilter->SetBackgroundLevel(backgroundLevel);
  resliceFilter->OptimizationOn();

  switch (interpolationType)
    {
    case vtkEMSegmentMRMLManager::InterpolationNearestNeighbor:
      resliceFilter->SetInterpolationModeToNearestNeighbor();
      break;
    case vtkEMSegmentMRMLManager::InterpolationCubic:
      resliceFilter->SetInterpolationModeToCubic();
      break;
    case vtkEMSegmentMRMLManager::InterpolationLinear:
    default:
      resliceFilter->SetInterpolationModeToLinear();
    }

  resliceFilter->Update();
  outputImageData->ShallowCopy(resliceFilter->GetOutput());

  //
  // clean up
  outputIJKToRAS->Delete();
  inputRASToIJK->Delete();
  resliceFilter->Delete();
  totalTransform->Delete();
}

// Assume geometry is already specified, create
// outGrid(p) = postMultiply \circ inGrid \circ preMultiply (p)
//
// right now simplicity over speed.  Optimize later?
//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
ComposeGridTransform(vtkGridTransform* inGrid,
                     vtkMatrix4x4*     preMultiply,
                     vtkMatrix4x4*     postMultiply,
                     vtkGridTransform* outGrid)
{
  // iterate over output grid
  double inPt[4] = {0, 0, 0, 1};
  double pt[4]   = {0, 0, 0, 1};
  double* outDataPtr = 
    static_cast<double*>(outGrid->GetDisplacementGrid()->GetScalarPointer());  
  vtkIdType numOutputVoxels = outGrid->GetDisplacementGrid()->
    GetNumberOfPoints();

  for (vtkIdType i = 0; i < numOutputVoxels; ++i)
    {
    outGrid->GetDisplacementGrid()->GetPoint(i, inPt);
    preMultiply->MultiplyPoint(inPt, pt);
    inGrid->TransformPoint(pt, pt);
    postMultiply->MultiplyPoint(pt, pt);
    
    *outDataPtr++ = pt[0] - inPt[0];
    *outDataPtr++ = pt[1] - inPt[1];
    *outDataPtr++ = pt[2] - inPt[2];
    }
}

//
// A Slicer3 wrapper around vtkImageReslice.  Reslice the image data
// from inputVolumeNode into outputVolumeNode with the output image
// geometry specified by outputVolumeGeometryNode.  Optionally specify
// a transform.  The reslice transorm will be:
//
// outputIJK->outputRAS->(outputRASToInputRASTransform)->inputRAS->inputIJK
//
//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
SlicerImageResliceWithGrid(vtkMRMLVolumeNode* inputVolumeNode,
                           vtkMRMLVolumeNode* outputVolumeNode,
                           vtkMRMLVolumeNode* outputVolumeGeometryNode,
                           vtkGridTransform* outputRASToInputRASTransform,
                           int interpolationType,
                           double backgroundLevel)
{
  vtkImageData* inputImageData  = inputVolumeNode->GetImageData();
  vtkImageData* outputImageData = outputVolumeNode->GetImageData();
  vtkImageData* outputGeometryData = NULL;
  if (outputVolumeGeometryNode != NULL)
    {
    outputGeometryData = outputVolumeGeometryNode->GetImageData();
    }

  vtkImageReslice* resliceFilter = vtkImageReslice::New();

  //
  // set inputs
  resliceFilter->SetInput(inputImageData);

  //
  // create total transform
  vtkTransformToGrid* gridSource = vtkTransformToGrid::New();
  vtkIdentityTransform* idTransform = vtkIdentityTransform::New();
  gridSource->SetInput(idTransform);
  //gridSource->SetGridScalarType(VTK_FLOAT);
  idTransform->Delete();

  //
  // set geometry
  if (outputGeometryData != NULL)
    {
    resliceFilter->SetInformationInput(outputGeometryData);
    outputVolumeNode->CopyOrientation(outputVolumeGeometryNode);

    gridSource->SetGridExtent(outputGeometryData->GetExtent());
    gridSource->SetGridSpacing(outputGeometryData->GetSpacing());
    gridSource->SetGridOrigin(outputGeometryData->GetOrigin());
    }
  else
    {
    gridSource->SetGridExtent(outputImageData->GetExtent());
    gridSource->SetGridSpacing(outputImageData->GetSpacing());
    gridSource->SetGridOrigin(outputImageData->GetOrigin());
    }
  gridSource->Update();
  vtkGridTransform* totalTransform = vtkGridTransform::New();
  totalTransform->SetDisplacementGrid(gridSource->GetOutput());
//  totalTransform->SetInterpolationModeToCubic();
  gridSource->Delete();
  
  //
  // fill in total transform
  // ijk of output -> RAS -> XFORM -> RAS -> ijk of input
  vtkMatrix4x4* outputIJKToRAS  = vtkMatrix4x4::New();
  outputVolumeNode->GetIJKToRASMatrix(outputIJKToRAS);
  vtkMatrix4x4* inputRASToIJK = vtkMatrix4x4::New();
  inputVolumeNode->GetRASToIJKMatrix(inputRASToIJK);
  vtkEMSegmentLogic::ComposeGridTransform(outputRASToInputRASTransform,
                                          outputIJKToRAS,
                                          inputRASToIJK,
                                          totalTransform);
  resliceFilter->SetResliceTransform(totalTransform);

  //
  // resample the image
  resliceFilter->SetBackgroundLevel(backgroundLevel);
  resliceFilter->OptimizationOn();

  switch (interpolationType)
    {
    case vtkEMSegmentMRMLManager::InterpolationNearestNeighbor:
      resliceFilter->SetInterpolationModeToNearestNeighbor();
      break;
    case vtkEMSegmentMRMLManager::InterpolationCubic:
      resliceFilter->SetInterpolationModeToCubic();
      break;
    case vtkEMSegmentMRMLManager::InterpolationLinear:
    default:
      resliceFilter->SetInterpolationModeToLinear();
    }

  resliceFilter->Update();
  outputImageData->ShallowCopy(resliceFilter->GetOutput());

  //
  // clean up
  outputIJKToRAS->Delete();
  inputRASToIJK->Delete();
  resliceFilter->Delete();
  totalTransform->Delete();
}

void
vtkEMSegmentLogic::
SlicerRigidRegister(vtkMRMLVolumeNode* fixedVolumeNode,
                    vtkMRMLVolumeNode* movingVolumeNode,
                    vtkMRMLVolumeNode* outputVolumeNode,
                    vtkTransform* fixedRASToMovingRASTransform,
                    int imageMatchType,
                    int interpolationType,
                    double backgroundLevel)
{
  vtkRigidRegistrator* registrator = vtkRigidRegistrator::New();

  // set fixed image ------
  registrator->SetFixedImage(fixedVolumeNode->GetImageData());
  vtkMatrix4x4* IJKToRASMatrixFixed = vtkMatrix4x4::New();
  fixedVolumeNode->GetIJKToRASMatrix(IJKToRASMatrixFixed);
  registrator->SetFixedIJKToXYZ(IJKToRASMatrixFixed);
  IJKToRASMatrixFixed->Delete();
    
  // set moving image ------
  registrator->SetMovingImage(movingVolumeNode->GetImageData());
  vtkMatrix4x4* IJKToRASMatrixMoving = vtkMatrix4x4::New();
  movingVolumeNode->GetIJKToRASMatrix(IJKToRASMatrixMoving);
  registrator->SetMovingIJKToXYZ(IJKToRASMatrixMoving);
  IJKToRASMatrixMoving->Delete();

  // set parameters ------  
  switch (imageMatchType)
    {
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationCenters:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetNumberOfIterations(0);      
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidNCCSlow:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetMetricComputationSamplingRatio(0.8);
      registrator->SetNumberOfIterations(100);
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidMMISlow:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetMetricComputationSamplingRatio(0.8);
      registrator->SetNumberOfIterations(100);
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidNCCFast:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(5);
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidMMIFast:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(5);
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidNCC:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(10);
      break;
    case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidMMI:
    default:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(10);
      break;
    }

  registrator->SetTransformInitializationTypeToImageCenters();

  switch (interpolationType)
    {
    case vtkEMSegmentMRMLManager::InterpolationNearestNeighbor:
      registrator->SetIntensityInterpolationTypeToNearestNeighbor();
      break;
    case vtkEMSegmentMRMLManager::InterpolationCubic:
      registrator->SetIntensityInterpolationTypeToCubic();
      break;
    case vtkEMSegmentMRMLManager::InterpolationLinear:
    default:
      registrator->SetIntensityInterpolationTypeToLinear();
    }

  try
    {
    //
    // run registration
    registrator->RegisterImages();
    fixedRASToMovingRASTransform->DeepCopy(registrator->GetTransform());

    if (outputVolumeNode != NULL)
      {
      //
      // resample moving image
      vtkEMSegmentLogic::SlicerImageReslice(movingVolumeNode, 
                                            outputVolumeNode, 
                                            fixedVolumeNode, 
                                            fixedRASToMovingRASTransform,
                                            interpolationType,
                                            backgroundLevel);
      }
    }
  catch (...)
    {
    std::cerr << "Failed to register images!!!" << std::endl;
    }
    
  //
  // clean up
  registrator->Delete();
}

void
vtkEMSegmentLogic::
SlicerBSplineRegister(vtkMRMLVolumeNode* fixedVolumeNode,
                      vtkMRMLVolumeNode* movingVolumeNode,
                      vtkMRMLVolumeNode* outputVolumeNode,
                      vtkGridTransform* fixedRASToMovingRASTransform,
                      vtkTransform* fixedRASToMovingRASAffineTransform,
                      int imageMatchType,
                      int interpolationType,
                      double backgroundLevel)
{
  vtkBSplineRegistrator* registrator = vtkBSplineRegistrator::New();
  
  // set fixed image ------
  registrator->SetFixedImage(fixedVolumeNode->GetImageData());
  vtkMatrix4x4* IJKToRASMatrixFixed = vtkMatrix4x4::New();
  fixedVolumeNode->GetIJKToRASMatrix(IJKToRASMatrixFixed);
  registrator->SetFixedIJKToXYZ(IJKToRASMatrixFixed);
  IJKToRASMatrixFixed->Delete();
    
  // set moving image ------
  registrator->SetMovingImage(movingVolumeNode->GetImageData());
  vtkMatrix4x4* IJKToRASMatrixMoving = vtkMatrix4x4::New();
  movingVolumeNode->GetIJKToRASMatrix(IJKToRASMatrixMoving);
  registrator->SetMovingIJKToXYZ(IJKToRASMatrixMoving);
  IJKToRASMatrixMoving->Delete();

  // set parameters ------  
  switch (imageMatchType)
    {
    case 
      vtkEMSegmentMRMLManager
      ::AtlasToTargetDeformableRegistrationBSplineNCCSlow:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.8);
      registrator->SetNumberOfIterations(100);
      break;
    case 
      vtkEMSegmentMRMLManager
      ::AtlasToTargetDeformableRegistrationBSplineMMISlow:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.8);
      registrator->SetNumberOfIterations(100);
      break;
    case 
      vtkEMSegmentMRMLManager
      ::AtlasToTargetDeformableRegistrationBSplineNCCFast:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.2);
      registrator->SetNumberOfIterations(5);
      break;
    case 
      vtkEMSegmentMRMLManager
      ::AtlasToTargetDeformableRegistrationBSplineMMIFast:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.2);
      registrator->SetNumberOfIterations(5);
      break;
    case 
      vtkEMSegmentMRMLManager::AtlasToTargetDeformableRegistrationBSplineNCC:
      registrator->SetImageToImageMetricToCrossCorrelation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(10);
      break;
    case 
      vtkEMSegmentMRMLManager::AtlasToTargetDeformableRegistrationBSplineMMI:
    default:
      registrator->SetImageToImageMetricToMutualInformation();
      registrator->SetNumberOfKnotPoints(5);
      registrator->SetMetricComputationSamplingRatio(0.3333);
      registrator->SetNumberOfIterations(10);
      break;
    }

  switch (interpolationType)
    {
    case vtkEMSegmentMRMLManager::InterpolationNearestNeighbor:
      registrator->SetIntensityInterpolationTypeToNearestNeighbor();
      break;
    case vtkEMSegmentMRMLManager::InterpolationCubic:
      registrator->SetIntensityInterpolationTypeToCubic();
      break;
    case vtkEMSegmentMRMLManager::InterpolationLinear:
    default:
      registrator->SetIntensityInterpolationTypeToLinear();
    }

  //
  // initialize with affine transform if specified
  if (fixedRASToMovingRASAffineTransform)
    {
    std::cerr << "   Setting bulk transform...";
    registrator->SetBulkTransform(fixedRASToMovingRASAffineTransform);
    std::cerr << "DONE" << std::endl;
    }

  try
    {
    //
    // run registration
    registrator->RegisterImages();
    fixedRASToMovingRASTransform->
      SetDisplacementGrid(registrator->GetTransform()->GetDisplacementGrid());

    if (outputVolumeNode != NULL)
      {
      std::cerr << "Resampling moving image..." << std::endl;
      vtkEMSegmentLogic::SlicerImageResliceWithGrid(movingVolumeNode, 
                                                    outputVolumeNode, 
                                                    fixedVolumeNode, 
                                                    fixedRASToMovingRASTransform,
                                                    interpolationType,
                                                    backgroundLevel);
      }
    std::cerr << "Resampling moving image DONE" << std::endl;
    }
  catch (...)
    {
    fixedRASToMovingRASTransform->SetDisplacementGrid(NULL);
    std::cerr << "Failed to register images!!!" << std::endl;
    }
    
  //
  // clean up
  registrator->Delete();
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
StartPreprocessingTargetToTargetRegistration()
{
  std::cerr << " EMSEG: Starting target-to-target registration..." 
            << std::endl;
  
  // get a pointer to the mrml manager for easy access
  vtkEMSegmentMRMLManager* m = this->MRMLManager;
  
  // get input target from working node
  vtkMRMLEMSTargetNode* normalizedTarget = 
    m->GetWorkingDataNode()->GetNormalizedTargetNode();
  if (normalizedTarget == NULL)
    {
    vtkWarningMacro("Normalized target node is null, aborting!");
    return false;
    }
  
  // check that global parameters exist
  if (!this->MRMLManager->GetGlobalParametersNode())
    {
    vtkWarningMacro("Global parameters node is null, aborting!");
    return false;
    }
  
  // set up the aligned target node
  vtkMRMLEMSTargetNode* alignedTarget = 
    m->GetWorkingDataNode()->GetAlignedTargetNode();
  if (!alignedTarget)
    {
    // clone intput to new aligned target node
    std::cerr << "  Cloning target node...";
    alignedTarget = m->CloneTargetNode(normalizedTarget, "AlignedTarget");
    std::cerr << "  Number of images is: " 
              << alignedTarget->GetNumberOfVolumes() << "..." << std::endl;
    m->GetWorkingDataNode()->
      SetAlignedTargetNodeID(alignedTarget->GetID());
    std::cerr << "Done." << std::endl;
    }
  else
    {
    if (!m->GetUpdateIntermediateData())
      {
      std::cerr << "  Using current target-to-target registered images." 
                << std::endl;
      return true;
      }
    }
  
  //
  // check that the number of target images did not change
  // !!! todo !!!
  
  //
  // apply registration
  // align image i with image 0
  int fixedTargetImageIndex = 0;
  vtkMRMLVolumeNode* fixedVolumeNode = 
    alignedTarget->GetNthVolumeNode(fixedTargetImageIndex);
  vtkImageData* fixedImageData = fixedVolumeNode->GetImageData();
  
  for (int i = 0; i < alignedTarget->GetNumberOfVolumes(); ++i)
    {
      std::cerr << "  Target image " << i << "...";

    if (i == fixedTargetImageIndex)
      {
        std::cerr <<  "Skipping fixed target image." << std::endl;
      continue;
      }

    //
    // get image data
    vtkMRMLVolumeNode* movingVolumeNode = 
      normalizedTarget->GetNthVolumeNode(i);
    vtkImageData* movingImageData = movingVolumeNode->GetImageData();
    vtkMRMLVolumeNode* outputVolumeNode = 
      alignedTarget->GetNthVolumeNode(i);
    vtkImageData* outImageData = outputVolumeNode->GetImageData(); 
    
    if (fixedImageData == NULL)
      {
      vtkWarningMacro("Fixed image is null, skipping: " << i);
      return false;
      }
    if (movingImageData == NULL)
      {
      vtkWarningMacro("Moving image is null, skipping: " << i);
      return false;
      }
    if (outImageData == NULL)
      {
      vtkWarningMacro("Registration output image is null, skipping: " << i);
      return false;
      }

    //
    // guess background level    
    double backgroundLevel = 0;
    switch (movingVolumeNode->GetImageData()->GetScalarType())
      {  
      vtkTemplateMacro(backgroundLevel = (GuessRegistrationBackgroundLevel<VTK_TT>(movingVolumeNode->GetImageData())););
      }
    std::cerr << "   Guessed background level: " << backgroundLevel
              << std::endl;

    //
    // apply rigid registration
    if (this->MRMLManager->GetEnableTargetToTargetRegistration())
      {
      vtkTransform* fixedRASToMovingRASTransform = vtkTransform::New();
      vtkEMSegmentLogic::
        SlicerRigidRegister
        (fixedVolumeNode,
         movingVolumeNode,
         outputVolumeNode,
         fixedRASToMovingRASTransform,
         vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationRigidMMI,
         vtkEMSegmentMRMLManager::InterpolationLinear,
         backgroundLevel);

      std::cerr << "  Target-to-target transform (fixedRAS -->> movingRAS): " 
                << std::endl;
      for (unsigned int r = 0; r < 4; ++r)
        {
        std::cerr << "   ";
        for (unsigned int c = 0; c < 4; ++c)
          {
          std::cerr 
            << fixedRASToMovingRASTransform->GetMatrix()->GetElement(r,c)
            << "   ";
          }
        std::cerr << std::endl;
        }
      fixedRASToMovingRASTransform->Delete();
      }
    else
      {
      std::cerr << "  Skipping registration of target image " 
                << i << "." << std::endl;

      if (!vtkEMSegmentLogic::
          IsVolumeGeometryEqual(fixedVolumeNode, outputVolumeNode))
        {
        std::cerr << "Warning: Target-to-target registration skipped but "
                  << "target images have differenent geometries. "
                  << std::endl
                  << "Suggestion: If you are not positive that your images are "
                  << "aligned, you should enable target-to-target registration."
                  << std::endl
                  << "Resampling target image " << i << "...";
        vtkEMSegmentLogic::
          SlicerImageReslice(movingVolumeNode, 
                             outputVolumeNode, 
                             fixedVolumeNode,
                             NULL,
                             vtkEMSegmentMRMLManager::InterpolationLinear,
                             backgroundLevel);        
        std::cerr << "DONE" << std::endl;
        }
      }
    }    
  std::cerr << " EMSEG: Target-to-target registration complete." << std::endl;
  
  // intensity statistics, if computed from data, must be updated
  m->UpdateIntensityDistributions();

  // everything was OK
  return true;
}

//----------------------------------------------------------------------------
bool
vtkEMSegmentLogic::
StartPreprocessingAtlasToTargetRegistration()
{
  std::cerr << " EMSEG: Starting atlas-to-target registration..." << std::endl;

  // get a pointer to the mrml manager for easy access
  vtkEMSegmentMRMLManager* m = this->MRMLManager;

  // get input target from working node
  vtkMRMLEMSTargetNode* alignedTarget = 
    m->GetWorkingDataNode()->GetAlignedTargetNode();
  if (alignedTarget == NULL)
    {
    vtkWarningMacro("Aligned target node is null, aborting!");
    return false;
    }

  // get input atlas from working node
  vtkMRMLEMSAtlasNode* inputAtlas = 
    m->GetWorkingDataNode()->GetInputAtlasNode();
  if (inputAtlas == NULL)
    {
    vtkWarningMacro("Input atlas node is null, aborting!");
    return false;
    }

  // check that global parameters exist
  if (!m->GetGlobalParametersNode())
    {
    vtkWarningMacro("Global parameters node is null, aborting!");
    return false;
    }

  // check that an atlas was selected for registration
  std::string atlasRegistrationVolumeKey(m->GetGlobalParametersNode()->
                                         GetRegistrationAtlasVolumeKey());
  int atlasRegistrationVolumeIndex = 
    inputAtlas->GetIndexByKey(atlasRegistrationVolumeKey.c_str());
  
  // set up the aligned atlas node
  vtkMRMLEMSAtlasNode* alignedAtlas = 
    m->GetWorkingDataNode()->GetAlignedAtlasNode();
  if (!alignedAtlas)
    {
    // clone intput to new aligned atlas node
    std::cerr << "  Cloning atlas node...";
    alignedAtlas = m->CloneAtlasNode(inputAtlas, "AlignedAtlas");
    std::cerr << "Done." << std::endl;
    std::cerr << "  Node is " << (alignedAtlas ? "Non-null" : "Null")
              << std::endl;
    std::cerr << "  Number of images is: " 
              << alignedAtlas->GetNumberOfVolumes() << std::endl;
    m->GetWorkingDataNode()->
      SetAlignedAtlasNodeID(alignedAtlas->GetID());
    }
  else
    {
    if (!m->GetUpdateIntermediateData())
      {
      std::cerr << "  Using current atlas-to-target registered images." 
                << std::endl;
      return true;
      }
    }
  
  //
  // check that the number of target images did not change
  // !!! todo !!!

  int fixedTargetImageIndex = 0;
  vtkMRMLVolumeNode* fixedTargetVolumeNode = 
    alignedTarget->GetNthVolumeNode(fixedTargetImageIndex);
  vtkImageData* fixedTargetImageData = fixedTargetVolumeNode->GetImageData();

  vtkTransform* fixedRASToMovingRASTransformAffine = vtkTransform::New();
  vtkGridTransform* fixedRASToMovingRASTransformDeformable = NULL;

  if (m->GetRegistrationAffineType() != 
      vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationOff ||
      m->GetRegistrationDeformableType() !=       
      vtkEMSegmentMRMLManager::AtlasToTargetDeformableRegistrationOff)
    {
    if (atlasRegistrationVolumeIndex < 0)
      {
      vtkWarningMacro
        ("Attempt to register atlas image but no atlas image selected!");
      return false;
      }

    // 
    // get moving, and output volume nodes    
    vtkMRMLVolumeNode* movingAtlasVolumeNode = 
      inputAtlas->GetNthVolumeNode(atlasRegistrationVolumeIndex);
    vtkImageData* movingAtlasImageData = movingAtlasVolumeNode->GetImageData();
    
    vtkMRMLVolumeNode* outputAtlasVolumeNode = 
      alignedAtlas->GetNthVolumeNode(atlasRegistrationVolumeIndex);
    vtkImageData* outAtlasImageData = outputAtlasVolumeNode->GetImageData(); 
    
    if (fixedTargetImageData == NULL)
      {
      vtkErrorMacro("Fixed image is null, skipping");
      return false;
      }
    if (movingAtlasImageData == NULL)
      {
      vtkErrorMacro("Moving image is null, skipping");
      return false;
      }
    if (outAtlasImageData == NULL)
      {
      vtkErrorMacro("Registration output is null, skipping");
      return false;
      }

    // affine registration
    switch (m->GetRegistrationAffineType())
      {
      case vtkEMSegmentMRMLManager::AtlasToTargetAffineRegistrationOff:
        std::cerr << "  Skipping affine registration of atlas image." 
                  << std::endl;
        break;
      default:
        // do rigid registration
        std::cerr << "  Registering atlas image rigid..." << std::endl;
        vtkEMSegmentLogic::
          SlicerRigidRegister(fixedTargetVolumeNode,
                              movingAtlasVolumeNode,
                              NULL,
                              fixedRASToMovingRASTransformAffine,
                              m->GetRegistrationAffineType(),
                              m->GetRegistrationInterpolationType(),
                              0);
        
        std::cerr << "  Atlas-to-target transform (fixedRAS -->> movingRAS): " 
                  << std::endl;
        for (unsigned int r = 0; r < 4; ++r)
          {
          std::cerr << "   ";
          for (unsigned int c = 0; c < 4; ++c)
            {
            std::cerr 
              << fixedRASToMovingRASTransformAffine->GetMatrix()->GetElement(r,c)
              << "   ";
            }
          std::cerr << std::endl;
          }
        break;
      }

    // deformable registration
    switch (m->GetRegistrationDeformableType())
      {
      case vtkEMSegmentMRMLManager::
        AtlasToTargetDeformableRegistrationOff:
        std::cerr << "  Skipping deformable registration of atlas image." 
                  << std::endl;
        break;
      default:
        // do deformable registration
        std::cerr << "  Registering atlas image B-Spline..." << std::endl;
        fixedRASToMovingRASTransformDeformable = vtkGridTransform::New();
        fixedRASToMovingRASTransformDeformable->SetInterpolationModeToCubic();
        vtkEMSegmentLogic::
          SlicerBSplineRegister(fixedTargetVolumeNode,
                                movingAtlasVolumeNode,
                                NULL,
                                fixedRASToMovingRASTransformDeformable,
                                fixedRASToMovingRASTransformAffine,
                                m->GetRegistrationDeformableType(),
                                m->GetRegistrationInterpolationType(),
                                0);
        break;
      }
    }

  //
  // resample all the atlas images using the same target->atlas transform
  for (int i = 0; i < alignedAtlas->GetNumberOfVolumes(); ++i)
    {
    //
    // get image data
    vtkMRMLVolumeNode* movingAtlasVolumeNode = inputAtlas->GetNthVolumeNode(i);
    vtkImageData* movingAtlasImageData = movingAtlasVolumeNode->GetImageData();
    vtkMRMLVolumeNode* outputAtlasVolumeNode = 
      alignedAtlas->GetNthVolumeNode(i);
    vtkImageData* outAtlasImageData = outputAtlasVolumeNode->GetImageData(); 

    if (movingAtlasImageData == NULL)
      {
      vtkErrorMacro("Moving image is null, skipping: " << i);
      return false;
      }
    if (outAtlasImageData == NULL)
      {
      vtkErrorMacro("Registration output is null, skipping: " << i);
      return false;
      }

    std::cerr << "  Resampling atlas image " << i << "..." << std::endl;

    //
    // guess background level    
    double backgroundLevel = 0;
    switch (movingAtlasVolumeNode->GetImageData()->GetScalarType())
      {  
      vtkTemplateMacro(backgroundLevel = (GuessRegistrationBackgroundLevel<VTK_TT>(movingAtlasVolumeNode->GetImageData())););
      }
    std::cerr << "   Guessed background level: " << backgroundLevel
              << std::endl;

    //
    // resample moving image
    if (fixedRASToMovingRASTransformDeformable != NULL)
      {
      vtkEMSegmentLogic::
        SlicerImageResliceWithGrid(movingAtlasVolumeNode, 
                                   outputAtlasVolumeNode, 
                                   fixedTargetVolumeNode,
                                   fixedRASToMovingRASTransformDeformable,
                                   m->GetRegistrationInterpolationType(),
                                   backgroundLevel);
      }
    else
      {
      vtkEMSegmentLogic::
        SlicerImageReslice(movingAtlasVolumeNode, 
                           outputAtlasVolumeNode, 
                           fixedTargetVolumeNode,
                           fixedRASToMovingRASTransformAffine,
                           m->GetRegistrationInterpolationType(),
                           backgroundLevel);
      }
    }    
  //
  // clean up
  fixedRASToMovingRASTransformAffine->Delete();
  if (fixedRASToMovingRASTransformDeformable)
    {
    fixedRASToMovingRASTransformDeformable->Delete();
    }
  std::cerr << " EMSEG: Atlas-to-target registration complete." << std::endl;

  // everything was OK
  return true;
}

//----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
StartSegmentation()
{
  //
  // make sure preprocessing is up to date
  //
  std::cerr << "EMSEG: Start preprocessing..." << std::endl;
  bool preprocessingOK = this->StartPreprocessing();
  std::cerr << "EMSEG: Preprocessing complete." << std::endl;

  if (!preprocessingOK)
    {
    vtkErrorMacro("Preprocessing Failed!  Aborting Segmentation.");
    return;
    }

  //
  // make sure we're ready to start
  //

  // find output volume
  if (!this->MRMLManager->GetSegmenterNode())
    {
    vtkErrorMacro("Segmenter node is null---aborting segmentation.");
    return;
    }
  vtkMRMLScalarVolumeNode *outVolume = 
    this->MRMLManager->GetOutputVolumeNode();
  if (outVolume == NULL)
    {
    vtkErrorMacro("No output volume found---aborting segmentation.");
    return;
    }

  //
  // Copy RASToIJK matrix, and other attributes from input to
  // output. Use first target volume as source for this data.
  //
  
  // get attributes from first target input volume
  const char* inMRLMID = 
    this->MRMLManager->GetTargetNode()->GetNthVolumeNodeID(0);
  vtkMRMLScalarVolumeNode *inVolume = vtkMRMLScalarVolumeNode::
    SafeDownCast(this->GetMRMLScene()->GetNodeByID(inMRLMID));
  if (inVolume == NULL)
    {
    vtkErrorMacro("Can't get first target image.");
    return;
    }

  outVolume->CopyOrientation(inVolume);
  outVolume->SetAndObserveTransformNodeID(inVolume->GetTransformNodeID());

  //
  // create segmenter class
  //
  vtkImageEMLocalSegmenter* segmenter = vtkImageEMLocalSegmenter::New();
  if (segmenter == NULL)
    {
    vtkErrorMacro("Could not create vtkImageEMLocalSegmenter pointer");
    return;
    }

  //
  // copy mrml data to segmenter class
  //
  vtkstd::cerr << "EMSEG: Copying data to algorithm class...";
  this->CopyDataToSegmenter(segmenter);
  vtkstd::cerr << "DONE" << vtkstd::endl;

  if (this->GetDebug())
  {
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
    segmenter->PrintSelf(vtkstd::cerr, 0);
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
    vtkstd::cerr << "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" << vtkstd::endl;
  }

  //
  // start segmentation
  //
  try 
    {
    vtkstd::cerr << "[Start] Segmentation algorithm..." 
                 << vtkstd::endl;
    segmenter->Update();
    vtkstd::cerr << "[Done]  Segmentation algorithm." << vtkstd::endl;
    }
  catch (std::exception e)
    {
    vtkErrorMacro("Exception thrown during segmentation: " << e.what());
    }

  if (this->GetDebug())
  {
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
    segmenter->PrintSelf(vtkstd::cerr, 0);
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
    vtkstd::cerr << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << vtkstd::endl;
  }

  //
  // copy result to output volume
  //
  
  // set ouput of the filter to VolumeNode's ImageData
  vtkImageData* image = vtkImageData::New(); 
  image->ShallowCopy(segmenter->GetOutput());
  outVolume->SetAndObserveImageData(image);
  image->Delete();
  // make sure the output volume is a labelmap
  if (!outVolume->GetLabelMap())
  {
    vtkWarningMacro("Changing output image to labelmap");
    outVolume->LabelMapOn();
  }
  outVolume->SetModifiedSinceRead(1);

  //
  // clean up
  //
  segmenter->Delete();

  //
  // save intermediate results
  if (this->MRMLManager->GetSaveIntermediateResults())
    {
    std::cerr << "Saving intermediate results...";
    bool savedResults = this->SaveIntermediateResults();
    std::cerr << "DONE" << std::endl;
    if (!savedResults)
      {
      vtkErrorMacro("Error writing intermediate results");
      }
    }
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
PopulateTestingData()
{
  vtkDebugMacro("Begin populating test data");

  //
  // add some nodes to the hierarchy
  //
  vtkDebugMacro("Setting parameters for root node");
  double color[3];
  vtkIdType rootNodeID         = this->MRMLManager->GetTreeRootNodeID();
  this->MRMLManager->SetTreeNodeLabel(rootNodeID, "Root");
  this->MRMLManager->SetTreeNodeName(rootNodeID, "Root");
  color[0] = 1.0; color[1] = 0.0; color[2] = 0.0;
  this->MRMLManager->SetTreeNodeColor(rootNodeID, color);
  this->MRMLManager->SetTreeNodeSpatialPriorWeight(rootNodeID, 0.5);
  this->MRMLManager->SetTreeNodeClassProbability(rootNodeID, 0.5);
  this->MRMLManager->SetTreeNodeAlpha(rootNodeID, 0.5);
  this->MRMLManager->SetTreeNodePrintWeight(rootNodeID, 1);
  this->MRMLManager->SetTreeNodeStoppingConditionEMType(rootNodeID, 1);
  this->MRMLManager->SetTreeNodeStoppingConditionEMIterations(rootNodeID, 15);
  this->MRMLManager->SetTreeNodeStoppingConditionEMValue(rootNodeID, 0.5);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAType(rootNodeID, 2);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAIterations(rootNodeID, 16);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAValue(rootNodeID, 0.6);

  vtkDebugMacro("Setting parameters for background node");
  vtkIdType backgroundNodeID   = this->MRMLManager->AddTreeNode(rootNodeID);
  this->MRMLManager->SetTreeNodeLabel(backgroundNodeID, "Background");
  this->MRMLManager->SetTreeNodeName(backgroundNodeID, "Background");
  color[0] = 0.0; color[1] = 0.0; color[2] = 0.0;
  this->MRMLManager->SetTreeNodeColor(backgroundNodeID, color);
  this->MRMLManager->SetTreeNodeSpatialPriorWeight(backgroundNodeID, 0.4);
  this->MRMLManager->SetTreeNodeClassProbability(backgroundNodeID, 0.4);
  this->MRMLManager->SetTreeNodePrintWeight(backgroundNodeID, 1);

  vtkDebugMacro("Setting parameters for icc node");
  vtkIdType iccNodeID          = this->MRMLManager->AddTreeNode(rootNodeID);
  this->MRMLManager->SetTreeNodeLabel(iccNodeID, "ICC");
  this->MRMLManager->SetTreeNodeName(iccNodeID, "ICC");
  color[0] = 0.0; color[1] = 1.0; color[2] = 0.0;
  this->MRMLManager->SetTreeNodeColor(iccNodeID, color);
  this->MRMLManager->SetTreeNodeSpatialPriorWeight(iccNodeID, 0.3);
  this->MRMLManager->SetTreeNodeClassProbability(iccNodeID, 0.3);
  this->MRMLManager->SetTreeNodeAlpha(iccNodeID, 0.3);
  this->MRMLManager->SetTreeNodePrintWeight(iccNodeID, 1);
  this->MRMLManager->SetTreeNodeStoppingConditionEMType(iccNodeID, 0);
  this->MRMLManager->SetTreeNodeStoppingConditionEMIterations(iccNodeID, 13);
  this->MRMLManager->SetTreeNodeStoppingConditionEMValue(iccNodeID, 0.3);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAType(iccNodeID, 1);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAIterations(iccNodeID, 14);
  this->MRMLManager->SetTreeNodeStoppingConditionMFAValue(iccNodeID, 0.4);

  vtkDebugMacro("Setting parameters for grey matter node");
  vtkIdType greyMatterNodeID   = this->MRMLManager->AddTreeNode(iccNodeID);
  this->MRMLManager->SetTreeNodeLabel(greyMatterNodeID, "Grey Matter");
  this->MRMLManager->SetTreeNodeName(greyMatterNodeID, "Grey Matter");
  color[0] = 0.0; color[1] = 1.0; color[2] = 1.0;
  this->MRMLManager->SetTreeNodeColor(greyMatterNodeID, color);
  this->MRMLManager->SetTreeNodeSpatialPriorWeight(greyMatterNodeID, 0.2);
  this->MRMLManager->SetTreeNodeClassProbability(greyMatterNodeID, 0.2);
  this->MRMLManager->SetTreeNodePrintWeight(greyMatterNodeID, 1);

  vtkDebugMacro("Setting parameters for white matter node");
  vtkIdType whiteMatterNodeID  = this->MRMLManager->AddTreeNode(iccNodeID);
  this->MRMLManager->SetTreeNodeLabel(whiteMatterNodeID, "White Matter");
  this->MRMLManager->SetTreeNodeName(whiteMatterNodeID, "White Matter");
  color[0] = 1.0; color[1] = 1.0; color[2] = 0.0;
  this->MRMLManager->SetTreeNodeColor(whiteMatterNodeID, color);
  this->MRMLManager->SetTreeNodeSpatialPriorWeight(whiteMatterNodeID, 0.1);
  this->MRMLManager->SetTreeNodeClassProbability(whiteMatterNodeID, 0.1);
  this->MRMLManager->SetTreeNodePrintWeight(whiteMatterNodeID, 1);

  vtkDebugMacro("Setting parameters for csf node");
  vtkIdType csfNodeID  = this->MRMLManager->AddTreeNode(iccNodeID);
  this->MRMLManager->SetTreeNodeLabel(csfNodeID, "CSF");
  this->MRMLManager->SetTreeNodeName(csfNodeID, "CSF");

  //
  // set registration parameters
  //
  vtkDebugMacro("Setting registration parameters");
  this->MRMLManager->SetRegistrationAffineType(0);
  this->MRMLManager->SetRegistrationDeformableType(0);
  this->MRMLManager->SetRegistrationInterpolationType(1);

  //
  // set save parameters
  //
  vtkDebugMacro("Setting save parameters");
  this->MRMLManager->SetSaveWorkingDirectory("/tmp");
  this->MRMLManager->SetSaveTemplateFilename("/tmp/EMSTemplate.mrml");
  this->MRMLManager->SetSaveTemplateAfterSegmentation(1);
  this->MRMLManager->SetSaveIntermediateResults(1);
  this->MRMLManager->SetSaveSurfaceModels(1);
  
  this->MRMLManager->SetEnableMultithreading(1);
  this->SetProgressGlobalFractionCompleted(0.9);

  vtkDebugMacro("Done populating test data");
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
SpecialTestingFunction()
{
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyDataToSegmenter(vtkImageEMLocalSegmenter* segmenter)
{
  //
  // copy atlas related parameters to algorithm
  //
  vtkstd::cerr << "atlas data...";
  this->CopyAtlasDataToSegmenter(segmenter);

  //
  // copy target related parameters to algorithm
  //
  vtkstd::cerr << "target data...";
  this->CopyTargetDataToSegmenter(segmenter);

  //
  // copy global parameters to algorithm 
  //
  vtkstd::cerr << "global data...";
  this->CopyGlobalDataToSegmenter(segmenter);

  //
  // copy tree base parameters to algorithm
  //
  vtkstd::cerr << "tree data...";
  vtkImageEMLocalSuperClass* rootNode = vtkImageEMLocalSuperClass::New();
  this->CopyTreeDataToSegmenter(rootNode, 
                                this->MRMLManager->GetTreeRootNodeID());
  segmenter->SetHeadClass(rootNode);
  rootNode->Delete();
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyAtlasDataToSegmenter(vtkImageEMLocalSegmenter* segmenter)
{
  segmenter->
    SetNumberOfTrainingSamples(this->MRMLManager->
                               GetAtlasNumberOfTrainingSamples());
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyTargetDataToSegmenter(vtkImageEMLocalSegmenter* segmenter)
{
  // !!! todo: TESTING HERE!!!
  vtkMRMLEMSTargetNode* workingTarget = 
    this->MRMLManager->GetWorkingDataNode()->GetWorkingTargetNode();
  unsigned int numTargetImages = workingTarget->GetNumberOfVolumes();
  segmenter->SetNumInputImages(numTargetImages);

  for (unsigned int i = 0; i < numTargetImages; ++i)
    {
    std::string mrmlID = workingTarget->GetNthVolumeNodeID(i);
    vtkDebugMacro("Setting target image " << i << " mrmlID=" 
                  << mrmlID.c_str());

    vtkImageData* imageData = 
      workingTarget->GetNthVolumeNode(i)->GetImageData();

    std::cerr << "AddingTargetImage..." << std::endl;
    this->PrintImageInfo(imageData);
    imageData->Update();
    this->PrintImageInfo(imageData);

    segmenter->SetImageInput(i, imageData);
    }
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyGlobalDataToSegmenter(vtkImageEMLocalSegmenter* segmenter)
{
  if (this->MRMLManager->GetEnableMultithreading())
    {
    segmenter->
      SetDisableMultiThreading(0);
    }
  else
    {
    segmenter->
      SetDisableMultiThreading(1);
    }
  segmenter->SetPrintDir(this->MRMLManager->GetSaveWorkingDirectory());
  
  //
  // NB: In the algorithm code alpha is defined globally.  In this
  // logic, it is defined for each parent node.  For now copy alpha
  // from the root tree node. !!!todo!!!
  //
  vtkIdType rootNodeID = this->MRMLManager->GetTreeRootNodeID();
  segmenter->SetAlpha(this->MRMLManager->GetTreeNodeAlpha(rootNodeID));
                      
  //
  // NB: In the algorithm code smoothing widht and sigma parameters
  // are defined globally.  In this logic, they are defined for each
  // parent node.  For now copy parameters from the root tree
  // node. !!!todo!!!
  //
  segmenter->
    SetSmoothingWidth(this->MRMLManager->
                      GetTreeNodeSmoothingKernelWidth(rootNodeID));

  // type mismatch between logic and algorithm !!!todo!!!
  int intSigma = 
    vtkMath::Round(this->MRMLManager->
                   GetTreeNodeSmoothingKernelSigma(rootNodeID));
  segmenter->SetSmoothingSigma(intSigma);

  //
  // registration parameters
  //
  int algType = this->ConvertGUIEnumToAlgorithmEnumInterpolationType
    (this->MRMLManager->GetRegistrationInterpolationType());
  segmenter->SetRegistrationInterpolationType(algType);
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyTreeDataToSegmenter(vtkImageEMLocalSuperClass* node, vtkIdType nodeID)
{
  // need this here because the vtkImageEM* classes don't use
  // virtual functions and so failed initializations lead to
  // memory errors
  node->SetNumInputImages(this->MRMLManager->
                          GetTargetNumberOfSelectedVolumes());

  // copy generic tree node data to segmenter
  this->CopyTreeGenericDataToSegmenter(node, nodeID);
  
  // copy parent specific tree node data to segmenter
  this->CopyTreeParentDataToSegmenter(node, nodeID);

  // add children
  unsigned int numChildren = 
    this->MRMLManager->GetTreeNodeNumberOfChildren(nodeID);
  double totalProbability = 0.0;
  for (unsigned int i = 0; i < numChildren; ++i)
    {
    vtkIdType childID = this->MRMLManager->GetTreeNodeChildNodeID(nodeID, i);
    bool isLeaf = this->MRMLManager->GetTreeNodeIsLeaf(childID);

    if (isLeaf)
      {
      vtkImageEMLocalClass* childNode = vtkImageEMLocalClass::New();
      // need this here because the vtkImageEM* classes don't use
      // virtual functions and so failed initializations lead to
      // memory errors
      childNode->SetNumInputImages(this->MRMLManager->
                                   GetTargetNumberOfSelectedVolumes());
      this->CopyTreeGenericDataToSegmenter(childNode, childID);
      this->CopyTreeLeafDataToSegmenter(childNode, childID);
      node->AddSubClass(childNode, i);
      childNode->Delete();
      }
    else
      {
      vtkImageEMLocalSuperClass* childNode = vtkImageEMLocalSuperClass::New();
      this->CopyTreeDataToSegmenter(childNode, childID);
      node->AddSubClass(childNode, i);
      childNode->Delete();
      }

    totalProbability += 
      this->MRMLManager->GetTreeNodeClassProbability(childID);
    }

  if (totalProbability != 1.0)
    {
    vtkWarningMacro("Warning: child probabilities don't sum to unity for node "
                    << this->MRMLManager->GetTreeNodeName(nodeID)
                    << " they sum to " << totalProbability);
    }

  // update Markov matrices
  const unsigned int numDirections = 6;
  bool nodeHasMatrix = 
    this->MRMLManager->GetTreeClassInteractionNode(nodeID) != NULL;
  if (!nodeHasMatrix)
    {
    vtkWarningMacro("CIM not available, using identity.");
    }
  for (unsigned int d = 0; d < numDirections; ++d)
    {
    for (unsigned int r = 0; r < numChildren; ++r)
      {
      for (unsigned int c = 0; c < numChildren; ++c)
        {
        double val = nodeHasMatrix 
          ? this->MRMLManager->GetTreeNodeClassInteraction(nodeID, d, r, c)
          : (r == c ? 1.0 : 0.0);
        node->SetMarkovMatrix(val, d, c, r);
        }
      }
    }
  node->Update();
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyTreeGenericDataToSegmenter(vtkImageEMLocalGenericClass* node, 
                               vtkIdType nodeID)
{
  unsigned int numTargetImages = 
    this->MRMLManager->GetTargetNumberOfSelectedVolumes();

  //
  // Setup ROI.  If if looks bogus then use the default (entire image)
  bool useDefaultBoundary = false;
  int boundMin[3];
  int boundMax[3];

  // get dimensions of target image
  int targetImageDimensions[3];
  this->MRMLManager->GetTargetNode()->GetNthVolumeNode(0)->
    GetImageData()->GetDimensions(targetImageDimensions);

  this->MRMLManager->GetSegmentationBoundaryMin(boundMin);
  this->MRMLManager->GetSegmentationBoundaryMax(boundMax);
  // Specify boundary in 1-based, NOT 0-based as you might expect
  for (unsigned int i = 0; i < 3; ++i)
    {
    if (boundMin[i] <  1                   || 
        boundMin[i] >  targetImageDimensions[i]   ||
        boundMax[i] <  1                   ||
        boundMax[i] >  targetImageDimensions[i]   ||
        boundMax[i] <= boundMin[i])
      {
      useDefaultBoundary = true;
      break;
      }
    }
  if (useDefaultBoundary)
    {
    std::cerr 
      << std::endl
      << "Warning: the segmentation ROI was bogus, setting ROI to entire image"
      << std::endl;
    for (unsigned int i = 0; i < 3; ++i)
      {
      boundMin[i] = 1;
      boundMax[i] = targetImageDimensions[i];
      std::cerr << boundMin[i] << ", " << boundMax[i] << "   ";
      }
    std::cerr << std::endl;
    }

  node->SetSegmentationBoundaryMin(boundMin[0], boundMin[1], boundMin[2]);
  node->SetSegmentationBoundaryMax(boundMax[0], boundMax[1], boundMax[2]);
  
  node->SetProbDataWeight(this->MRMLManager->
                          GetTreeNodeSpatialPriorWeight(nodeID));

  node->SetTissueProbability(this->MRMLManager->
                             GetTreeNodeClassProbability(nodeID));

  node->SetPrintWeights(this->MRMLManager->GetTreeNodePrintWeight(nodeID));

  // set target input channel weights
  for (unsigned int i = 0; i < numTargetImages; ++i)
    {
    node->SetInputChannelWeights(this->MRMLManager->
                                 GetTreeNodeInputChannelWeight(nodeID, 
                                                               i), i);
    }

  //
  // registration related data
  //
  //!!!bcd!!!

  //
  // set probability data
  //

  // get working atlas
  // !!! error checking!
  vtkMRMLEMSAtlasNode* workingAtlas = 
    this->MRMLManager->GetWorkingDataNode()->GetWorkingAtlasNode();

  std::string atlasVolumeKey = this->MRMLManager->GetTreeParametersNode(nodeID)->GetSpatialPriorVolumeName();
  int atlasVolumeIndex       = workingAtlas->GetIndexByKey(atlasVolumeKey.c_str());

  if (atlasVolumeIndex >= 0)
    {
    vtkDebugMacro("Setting spatial prior: node=" 
                  << this->MRMLManager->GetTreeNodeLabel(nodeID));
    vtkImageData* imageData = workingAtlas->GetNthVolumeNode(atlasVolumeIndex)->GetImageData();
    node->SetProbDataPtr(imageData);
    }

  int exclude = 
    this->MRMLManager->GetTreeNodeExcludeFromIncompleteEStep(nodeID);
  node->SetExcludeFromIncompleteEStepFlag(exclude);
}


//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyTreeParentDataToSegmenter(vtkImageEMLocalSuperClass* node, 
                              vtkIdType nodeID)
{
  node->SetPrintFrequency (this->MRMLManager->
                           GetTreeNodePrintFrequency(nodeID));
  node->SetPrintBias      (this->MRMLManager->
                           GetTreeNodePrintBias(nodeID));
  node->SetPrintLabelMap  (this->MRMLManager->
                           GetTreeNodePrintLabelMap(nodeID));

  node->SetPrintEMLabelMapConvergence
    (this->MRMLManager->GetTreeNodePrintEMLabelMapConvergence(nodeID));
  node->SetPrintEMWeightsConvergence
    (this->MRMLManager->GetTreeNodePrintEMWeightsConvergence(nodeID));
  node->SetStopEMType(this->ConvertGUIEnumToAlgorithmEnumStoppingConditionType
                      (this->MRMLManager->
                      GetTreeNodeStoppingConditionEMType(nodeID)));
  node->SetStopEMValue(this->MRMLManager->
                       GetTreeNodeStoppingConditionEMValue(nodeID));
  node->SetStopEMMaxIter
    (this->MRMLManager->GetTreeNodeStoppingConditionEMIterations(nodeID));

  node->SetPrintMFALabelMapConvergence
    (this->MRMLManager->GetTreeNodePrintMFALabelMapConvergence(nodeID));
  node->SetPrintMFAWeightsConvergence
    (this->MRMLManager->GetTreeNodePrintMFAWeightsConvergence(nodeID));
  node->SetStopMFAType(this->ConvertGUIEnumToAlgorithmEnumStoppingConditionType
                       (this->MRMLManager->
                       GetTreeNodeStoppingConditionMFAType(nodeID)));
  node->SetStopMFAValue(this->MRMLManager->
                        GetTreeNodeStoppingConditionMFAValue(nodeID));
  node->SetStopMFAMaxIter
    (this->MRMLManager->GetTreeNodeStoppingConditionMFAIterations(nodeID));

  node->SetStopBiasCalculation
    (this->MRMLManager->GetTreeNodeBiasCalculationMaxIterations(nodeID));

  node->SetPrintShapeSimularityMeasure(0);         // !!!bcd!!!

  node->SetPCAShapeModelType(0);                   // !!!bcd!!!

  node->SetRegistrationIndependentSubClassFlag(0); // !!!bcd!!!
  node->SetRegistrationType(0);                    // !!!bcd!!!

  node->SetGenerateBackgroundProbability
    (this->MRMLManager->GetTreeNodeGenerateBackgroundProbability(nodeID));
}

//-----------------------------------------------------------------------------
void
vtkEMSegmentLogic::
CopyTreeLeafDataToSegmenter(vtkImageEMLocalClass* node, 
                            vtkIdType nodeID)
{
  unsigned int numTargetImages = 
    this->MRMLManager->GetTargetNumberOfSelectedVolumes();

  // this label describes the output intensity value for this class in
  // the segmentation result
  node->SetLabel(this->MRMLManager->GetTreeNodeIntensityLabel(nodeID));

  // set log mean and log covariance
  for (unsigned int r = 0; r < numTargetImages; ++r)
    {
    node->SetLogMu(this->MRMLManager->
                   GetTreeNodeDistributionLogMean(nodeID, r), r);

    for (unsigned int c = 0; c < numTargetImages; ++c)
      {
      node->SetLogCovariance(this->MRMLManager->
                             GetTreeNodeDistributionLogCovariance(nodeID,
                                                                  r, c), 
                             r, c);
      }
    }

  node->SetPrintQuality(this->MRMLManager->GetTreeNodePrintQuality(nodeID));
}

//-----------------------------------------------------------------------------
int
vtkEMSegmentLogic::
ConvertGUIEnumToAlgorithmEnumStoppingConditionType(int guiEnumValue)
{
  switch (guiEnumValue)
    {
    case (vtkEMSegmentMRMLManager::StoppingConditionIterations):
      return EMSEGMENT_STOP_FIXED;
    case (vtkEMSegmentMRMLManager::StoppingConditionLabelMapMeasure):
      return EMSEGMENT_STOP_LABELMAP;
    case (vtkEMSegmentMRMLManager::StoppingConditionWeightsMeasure):
      return EMSEGMENT_STOP_WEIGHTS;
    default:
      vtkErrorMacro("Unknown stopping condition type: " << guiEnumValue);
      return -1;
    }
}

//-----------------------------------------------------------------------------
int
vtkEMSegmentLogic::
ConvertGUIEnumToAlgorithmEnumInterpolationType(int guiEnumValue)
{
  switch (guiEnumValue)
    {
    case (vtkEMSegmentMRMLManager::InterpolationLinear):
      return EMSEGMENT_REGISTRATION_INTERPOLATION_LINEAR;
    case (vtkEMSegmentMRMLManager::InterpolationNearestNeighbor):
      return EMSEGMENT_REGISTRATION_INTERPOLATION_NEIGHBOUR;
    case (vtkEMSegmentMRMLManager::InterpolationCubic):
      // !!! not implemented
      vtkErrorMacro("Cubic interpolation not implemented: " << guiEnumValue);
      return -1;
    default:
      vtkErrorMacro("Unknown interpolation type: " << guiEnumValue);
      return -1;
    }
}
