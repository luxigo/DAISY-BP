/*

  Project  : DAISY in OpenCL
  Author   : Ioannis Panousis - ip223@bath.ac.uk
  Creation : February/2012

  File: oclDaisy.cpp

*/

#include "oclDaisy.h"
#include <omp.h>
#include "general.h"

char * writeInfofile(daisy_params * daisy, char * binaryfile);
double timeDiff(struct timeval start, struct timeval end);
void testFetchDaisy(daisy_params * daisy, ocl_constructs * daisyCl, cl_mem daisyBufferA, time_params * times);
float * generatePetalOffsets(float, int, short int);
int * generateTranspositionOffsets(int, int, float*, int, int*, int*);
long int verifyConvolutionY(float * inputData, float * outputData, int height, int width, float * filter, int filterSize, int downsample);
long int verifyConvolutionX(float * inputData, float * outputData, int height, int width, float * filter, int filterSize, int downsample);
long int verifyTransposeGradientsPartialNorm(float * inputData, float * outputData, int width, int height, int gradientsNo);
long int verifyTransposeDaisyPairs(daisy_params * daisy, float * transArray, float * daisyArray, int startRow, int endRow, float ** allPetalOffsets);

#ifndef FETCH_RANGE_START
#define FETCH_RANGE_START 512
#endif

// Maximum width that this TR_BLOCK_SIZE is effective on (assuming at least 
// TR_DATA_WIDTH rows should be allocated per block) is currently 16384

// Input 2D array
#define ARRAY_PADDING 64

// Configuration & special constants for slow kernel
#define TR_BLOCK_SIZE 512*512
#define TR_DATA_WIDTH 16
#define TR_PAIRS_SINGLE_ONLY -999
#define TR_PAIRS_OFFSET_WIDTH 1000

daisy_params * newDaisyParams(const char * filename, unsigned char* array, int height, int width,
                              short int cpuTransfer){

  daisy_params * params = (daisy_params*) malloc(sizeof(daisy_params));
  params->filename = (char*) malloc(sizeof(char) * 500);
  strcpy(params->filename,filename);
  params->array = array;
  params->height = height;
  params->width = width;
  params->gradientsNo = GRADIENTS_NO;
  params->regionPetalsNo = REGION_PETALS_NO;
  params->smoothingsNo = SMOOTHINGS_NO;
  params->totalPetalsNo = TOTAL_PETALS_NO;
  params->descriptors = NULL;
  params->descriptorLength = DESCRIPTOR_LENGTH;
  params->cpuTransfer = cpuTransfer;
  params->oclKernels = (ocl_daisy_kernels*) malloc(sizeof(ocl_daisy_kernels));
  params->oclKernels->kernelsNo = 20;
  *(params->oclKernels) = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
  params->buffers = (cl_mem*) malloc(sizeof(cl_mem) * 10);
  params->buffersSize = 0;

  return params;
}

void unpadDescriptorArray(daisy_params * daisy){

  float * array = daisy->descriptors;
  int paddingAdded = daisy->paddedWidth - daisy->width;
  printf("PaddingAdded: %d\n",paddingAdded);

  if(paddingAdded == 0) return;

  for(int row = 1; row < daisy->height; row++){
    
    int rewind = row * paddingAdded * daisy->descriptorLength;

    for(int i = row * daisy->paddedWidth * daisy->descriptorLength; i < (row+1) * daisy->paddedWidth * daisy->descriptorLength; i++){

      array[i - rewind] = array[i];

    }

  }

}

int daisyCleanUp(daisy_params * daisy, ocl_constructs * daisyCl){

  // do clean up

  for(int i = 0, buffersNo = daisy->buffersSize; i < buffersNo; i++, daisy->buffersSize--)
    clReleaseMemObject(daisy->buffers[i]);

  oclCleanUp(daisy->oclKernels,daisyCl,0);

  return 0;

}

int oclError(const char * function, const char * functionCall, int error){

  if(error){
    fprintf(stderr, "oclDaisy.cpp::%s %s failed: %d\n",function,functionCall,error);
    return error;
  }
  
  return 0;

}

int oclCleanUp(ocl_daisy_kernels * daisy, ocl_constructs * daisyCl, int error){

  // Release kernels
  if(daisy->denx      != NULL) { clReleaseKernel(daisy->denx); daisy->denx = NULL; }
  if(daisy->deny      != NULL) { clReleaseKernel(daisy->deny); daisy->deny = NULL; }
  if(daisy->grad      != NULL) { clReleaseKernel(daisy->grad); daisy->grad = NULL; }
  if(daisy->G0x       != NULL) { clReleaseKernel(daisy->G0x); daisy->G0x = NULL; }
  if(daisy->G0y       != NULL) { clReleaseKernel(daisy->G0y); daisy->G0y = NULL; }
  if(daisy->G1x       != NULL) { clReleaseKernel(daisy->G1x); daisy->G1x = NULL; }
  if(daisy->G1y       != NULL) { clReleaseKernel(daisy->G1y); daisy->G1y = NULL; }
  if(daisy->G2x       != NULL) { clReleaseKernel(daisy->G2x); daisy->G2x = NULL; }
  if(daisy->G2y       != NULL) { clReleaseKernel(daisy->G2y); daisy->G2y = NULL; }
  if(daisy->trans     != NULL) { clReleaseKernel(daisy->trans); daisy->trans = NULL; }
  if(daisy->transd    != NULL) { clReleaseKernel(daisy->transd); daisy->transd = NULL; }
  if(daisy->transdp   != NULL) { clReleaseKernel(daisy->transdp); daisy->transdp = NULL; }
  if(daisy->transds   != NULL) { clReleaseKernel(daisy->transds); daisy->transds = NULL; }
  if(daisy->fetchd    != NULL) { clReleaseKernel(daisy->fetchd); daisy->fetchd = NULL; }
  if(daisy->diffCoarse != NULL) { clReleaseKernel(daisy->diffCoarse); daisy->diffCoarse = NULL; }
  if(daisy->transposeRotations != NULL) { clReleaseKernel(daisy->transposeRotations); daisy->transposeRotations = NULL; }
  if(daisy->reduceMin != NULL) { clReleaseKernel(daisy->reduceMin); daisy->reduceMin = NULL; }
  if(daisy->reduceMinAll != NULL) { clReleaseKernel(daisy->reduceMinAll); daisy->reduceMinAll = NULL; }
  if(daisy->diffMiddle != NULL) { clReleaseKernel(daisy->diffMiddle); daisy->diffMiddle = NULL; }

  // Release command queues
  if(daisyCl->ioqueue != NULL) { clReleaseCommandQueue(daisyCl->ioqueue); daisyCl->ioqueue = NULL; }
  if(daisyCl->ooqueue != NULL) { clReleaseCommandQueue(daisyCl->ooqueue); daisyCl->ooqueue = NULL; }

  return error;

}

daisy_params * initDaisy(const char * filename, short int saveBinary){

  unsigned char * srcArray = NULL;
  int height, width;

  load_gray_image(filename, srcArray, height, width);

  if(height * width > 2048 * 2048){
    fprintf(stderr, "This implementation cannot yet handle larger image sizes than 2048*2048.\n\
                     Try to split your images in blocks or otherwise feel free to implement DAISY computation in parts :)");
    return NULL;
  }

  printf("HxW=%dx%d\n",height,width);

  return newDaisyParams(filename, srcArray, height, width, saveBinary);

}

int initOcl(daisy_params * daisy, ocl_constructs * daisyCl){

  cl_int error;

  // Prepare/Reuse platform, device, context, command queue
  cl_bool recreateBuffers = 0;

  error = buildCachedConstructs(daisyCl, &recreateBuffers);
  if(oclError("initOcl","buildCachedConstructs",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  // Pass preprocessor build options
//  const char options[128] = "-cl-mad-enable -cl-fast-relaxed-math -DFSC=14";    
  char * options = (char*) malloc(sizeof(char) * 500);
  sprintf(options, "-cl-mad-enable -cl-fast-relaxed-math -DDM_WGX=%d -DDM_WG_TARGETS_NO=%d -DDM_TARGETS_PER_LOOP=%d -DDM_SEARCH_WIDTH=%d -DDM_ROTATIONS_NO=%d", 
                     DM_WGX, DM_WG_TARGETS_NO, DM_TARGETS_PER_LOOP, DM_SEARCH_WIDTH, DM_ROTATIONS_NO);

  // Build denoising filter
  error = buildCachedProgram(daisyCl, "daisyKernels.cl", options);
  if(oclError("initOcl","buildCachedProgram",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
  
  if(daisyCl->program == NULL){
    fprintf(stderr, "oclDaisy.cpp::oclDaisy buildCachedProgram returned NULL, cannot continue\n");
    return oclCleanUp(daisy->oclKernels,daisyCl,1);
  }

  // Prepare the kernel
  daisy->oclKernels->denx = clCreateKernel(daisyCl->program, "convolve_denx", &error);
  daisy->oclKernels->deny = clCreateKernel(daisyCl->program, "convolve_deny", &error);
  if(oclError("initOcl","clCreateKernel (den)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  // Build gradient kernel
  daisy->oclKernels->grad = clCreateKernel(daisyCl->program, "gradients", &error);
  if(oclError("initOcl","clCreateKernel (grad)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
  
  daisy->oclKernels->G0x = clCreateKernel(daisyCl->program, "convolve_G0x", &error);
  daisy->oclKernels->G0y = clCreateKernel(daisyCl->program, "convolve_G0y", &error);
  if(oclError("initOcl","clCreateKernel (G0)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
  
  daisy->oclKernels->G1x = clCreateKernel(daisyCl->program, "convolve_G1x", &error);
  daisy->oclKernels->G1y = clCreateKernel(daisyCl->program, "convolve_G1y", &error);
  if(oclError("initOcl","clCreateKernel (G1)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
  
  daisy->oclKernels->G2x = clCreateKernel(daisyCl->program, "convolve_G2x", &error);
  daisy->oclKernels->G2y = clCreateKernel(daisyCl->program, "convolve_G2y", &error);
  if(oclError("initOcl","clCreateKernel(G2)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  daisy->oclKernels->trans = clCreateKernel(daisyCl->program, "transposeGradients", &error);
  if(oclError("initOcl","clCreateKernel (trans)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  //daisy->kernel_transd = clCreateKernel(daisyCl->program, "transposeDaisy", &error);
  daisy->oclKernels->transdp = clCreateKernel(daisyCl->program, "transposeDaisyPairs", &error);
  if(oclError("initOcl","clCreateKernel (transdp)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  daisy->oclKernels->transds = clCreateKernel(daisyCl->program, "transposeDaisySingles", &error);
  if(oclError("initOcl","clCreateKernel (transds)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  daisy->oclKernels->fetchd = clCreateKernel(daisyCl->program, "fetchDaisy", &error);
  if(oclError("initOcl","clCreateKernel (fetchd)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  return error;
}

void displayTimes(daisy_params * daisy, time_params * times){

  printf("convgrad: %.1f ms\n",timeDiff(times->startConvGrad,times->endConvGrad));
  printf("transANorm: %.1f ms\n",timeDiff(times->startTransGrad,times->endTransGrad));
  printf("transPinned: %.1f ms\n",times->transPinned);
  printf("transRam: %.1f ms\n",times->transRam);
  printf("daisyFull: %.1f ms\n",timeDiff(times->startFull,times->endFull));

}

char * writeInfofile(daisy_params * daisy, char * binaryfile){

  char * infofile = (char*)malloc(sizeof(char) * 300);
  sprintf(infofile,strcat(binaryfile,".info"));
    
  FILE * ff = fopen(infofile,"w");
  fprintf(ff,"Width = %d\n",daisy->width);
  fprintf(ff,"Height = %d\n",daisy->height);
  fprintf(ff,"Descriptor Length = %d\n",daisy->descriptorLength);
  fprintf(ff,"Datatype = %s\n","float");
  fprintf(ff,"Datastart = %d\n",4);
  fprintf(ff,"Recommended descriptor clip width on all borders = %d\n",15);

  fclose(ff);

  return infofile;

}

void saveToBinary(daisy_params * daisy){

  char * binaryfile = strcat(daisy->filename,".bdaisy");

  unpadDescriptorArray(daisy);

  kutility::save_binary(binaryfile, daisy->descriptors, daisy->height * daisy->width, daisy->descriptorLength, 1, kutility::TYPE_FLOAT);

  char * infoFilename = writeInfofile(daisy,binaryfile);

  printf("Binary: %s\nInfo File: %s\n", binaryfile, infoFilename);

}

int oclDaisy(daisy_params * daisy, ocl_constructs * daisyCl, time_params * times){

  cl_int error;

  daisy->paddedWidth = daisy->width + (ARRAY_PADDING - daisy->width % ARRAY_PADDING) % ARRAY_PADDING;
  daisy->paddedHeight = daisy->height + (ARRAY_PADDING - daisy->height % ARRAY_PADDING) % ARRAY_PADDING;

  float * inputArray = (float*)malloc(sizeof(float) * daisy->paddedWidth * daisy->paddedHeight * 8);

  int windowHeight = TR_DATA_WIDTH;
  int windowWidth  = TR_DATA_WIDTH;

  float sigmas[3] = {SIGMA_A,SIGMA_B,SIGMA_C};

  float * allPetalOffsets[daisy->smoothingsNo];
  int * allPairOffsets[3];
  int allPairOffsetsLengths[3];
  cl_mem allPairOffsetBuffers[3];

  for(int smoothingNo = 0; smoothingNo < daisy->smoothingsNo; smoothingNo++){

    int petalsNo = daisy->regionPetalsNo + (smoothingNo==0);

    float * petalOffsets = generatePetalOffsets(sigmas[smoothingNo], daisy->regionPetalsNo, (smoothingNo==0));

    allPetalOffsets[smoothingNo] = petalOffsets;

    int pairOffsetsLength, actualPairs;
    int * pairOffsets = generateTranspositionOffsets(windowHeight, windowWidth,
                                                     petalOffsets, petalsNo,
                                                     &pairOffsetsLength, &actualPairs);


    cl_mem pairOffsetBuffer = clCreateBuffer(daisyCl->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                             pairOffsetsLength * 4 * sizeof(int), (void*)pairOffsets, &error);

    oclError("oclDaisy","clCreateBuffer (pairOffset)",error);

    allPairOffsets[smoothingNo] = pairOffsets;
    allPairOffsetsLengths[smoothingNo] = pairOffsetsLength;
    allPairOffsetBuffers[smoothingNo] = pairOffsetBuffer;

  }

  //
  // Preparation for daisy transposition parameters and enqueue the time-consuming memory mapping
  //

  int daisyBlockWidth = daisy->paddedWidth;
  int daisyBlockHeight = min(TR_BLOCK_SIZE, daisy->paddedWidth * daisy->paddedHeight) / daisyBlockWidth;
  //printf("DaisyBlockHeight = %d\n",daisyBlockHeight);

  int totalSections = daisy->paddedHeight / daisyBlockHeight;

  // the height of the final block is taken care of just before the computation later on
  if(totalSections * daisyBlockHeight < daisy->paddedHeight) totalSections++;

  unsigned long int daisySectionSize = daisyBlockWidth * daisyBlockHeight * daisy->descriptorLength * sizeof(float);

  //
  // End of parameter preparation
  // 

  //
  // DAISY to CPU transfer setup
  //
  void * daisyDescriptorsSection;
  cl_mem hostPinnedDaisyDescriptors = NULL;
  if(daisy->cpuTransfer){

    hostPinnedDaisyDescriptors = clCreateBuffer(daisyCl->context, CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR, 
                                                daisySectionSize, NULL, &error);

    if(oclError("oclDaisy","clCreateBuffer (hostPinned)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

    daisyDescriptorsSection = (void*)clEnqueueMapBuffer(daisyCl->ioqueue, hostPinnedDaisyDescriptors, 0,
                                                        CL_MAP_WRITE, 0, daisySectionSize,
                                                        0, NULL, NULL, &error);

    if(oclError("oclDaisy","clEnqueueMapBuffer (daisySection)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

    if(totalSections == 1){

      // transfer only to pinned without doing the memcpy to non-pinned
      daisy->descriptors = (float*)daisyDescriptorsSection;

    }
    else{
    
      unsigned long int daisyDescriptorSize = daisy->paddedWidth * daisy->paddedHeight * 
                        daisy->descriptorLength * sizeof(float);

      if(daisy->descriptors == NULL){
        daisy->descriptors = (float*)malloc(daisyDescriptorSize);
      }
      else{
        daisy->descriptors = (float*)realloc(daisy->descriptors, daisyDescriptorSize);
      }
      

    }
    //printf("\nBlock Size calculated (HxW): %dx%d\n",daisyBlockHeight,daisyBlockWidth);
  
  }  
  //
  // end of DAISY to CPU transfer setup
  //

  clFinish(daisyCl->ioqueue);

  gettimeofday(&times->startFull,NULL);

  int paddedWidth  = daisy->paddedWidth;
  int paddedHeight = daisy->paddedHeight;

  long int memorySize = daisy->gradientsNo * (daisy->smoothingsNo+1) * 
                        paddedWidth * paddedHeight * sizeof(cl_float);

  cl_mem massBuffer = clCreateBuffer(daisyCl->context, CL_MEM_READ_WRITE,
                                     memorySize, (void*)NULL, &error);

  if(oclError("oclDaisy","clCreateBuffer (mass)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  //printf("massBuffer size = %ld (%ldMB)\n", memorySize, memorySize / (1024 * 1024));
  //printf("paddedWidth = %d, paddedHeight = %d\n", paddedWidth, paddedHeight);

  int filterDenSize = 5;
  float * filterDen = (float*)malloc(sizeof(float)*filterDenSize);
  kutility::gaussian_1d(filterDen,filterDenSize,SIGMA_DEN,0);

  int filterG0Size = 11;
  float * filterG0 = (float*)malloc(sizeof(float)*filterG0Size);
  kutility::gaussian_1d(filterG0,filterG0Size,sqrt(SIGMA_A * SIGMA_A - SIGMA_DEN * SIGMA_DEN),0);

  int filterG1Size = 23;
  float * filterG1 = (float*)malloc(sizeof(float)*filterG1Size);
  kutility::gaussian_1d(filterG1,filterG1Size,sqrt(SIGMA_B * SIGMA_B - SIGMA_A * SIGMA_A),0);

  int filterG2Size = 29;
  float * filterG2 = (float*)malloc(sizeof(float)*filterG2Size);
  kutility::gaussian_1d(filterG2,filterG2Size,sqrt(SIGMA_C * SIGMA_C - SIGMA_B * SIGMA_B),0);

  const int filterOffsets[4] = {0, filterDenSize, filterDenSize + filterG0Size,
                                   filterDenSize + filterG0Size + filterG1Size};

  cl_mem filterBuffer = clCreateBuffer(daisyCl->context, CL_MEM_READ_ONLY,
                                       (filterDenSize + filterG0Size + filterG1Size + filterG2Size) * sizeof(float),
                                       (void*)NULL, &error);

  clEnqueueWriteBuffer(daisyCl->ioqueue, filterBuffer, CL_FALSE,
                       filterOffsets[0] * sizeof(float), filterDenSize * sizeof(float), (void*)filterDen,
                       0, NULL, NULL);
  clEnqueueWriteBuffer(daisyCl->ioqueue, filterBuffer, CL_FALSE,
                       filterOffsets[1] * sizeof(float), filterG0Size * sizeof(float), (void*)filterG0,
                       0, NULL, NULL);
  clEnqueueWriteBuffer(daisyCl->ioqueue, filterBuffer, CL_FALSE,
                       filterOffsets[2] * sizeof(float), filterG1Size * sizeof(float), (void*)filterG1,
                       0, NULL, NULL);
  clEnqueueWriteBuffer(daisyCl->ioqueue, filterBuffer, CL_FALSE,
                       filterOffsets[3] * sizeof(float), filterG2Size * sizeof(float), (void*)filterG2,
                       0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueWriteBuffer (filters)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

//FIX: put pad in another function
  // Pad edges of input array for i) to fit the workgroup size ii) convolution halo - resample nearest pixel
  int i;
  for(i = 0; i < daisy->height; i++){
    int j;
    for(j = 0; j < daisy->width; j++)
      inputArray[i * paddedWidth + j] = daisy->array[i * daisy->width + j];
    for(j = daisy->width; j < paddedWidth; j++)
      inputArray[i * paddedWidth + j] = daisy->array[i * daisy->width + daisy->width-1];
  }
  for(i = daisy->height; i < paddedHeight; i++){
    int j;
    for(j = 0; j < paddedWidth; j++)
      inputArray[i * paddedWidth + j] = daisy->array[(daisy->height-1) * daisy->width + j];
  }

  error = clEnqueueWriteBuffer(daisyCl->ioqueue, massBuffer, CL_TRUE,
                               0, paddedWidth * paddedHeight * sizeof(float),
                               (void*)inputArray,
                               0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueWriteBuffer (inputArray)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  gettimeofday(&times->startConvGrad,NULL);

  // smooth with kernel size 7 (achieve sigma 1.6 from 0.5)
  size_t convWorkerSizeDenx[2] = {daisy->paddedWidth / 4, daisy->paddedHeight};
  size_t convGroupSizeDenx[2] = {16,8};

  // convolve X - A.0 to A.1
  clSetKernelArg(daisy->oclKernels->denx, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->denx, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->denx, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->denx, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->denx, 2, NULL, 
                                 convWorkerSizeDenx, convGroupSizeDenx, 0, 
                                 NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (denx)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  error = clFinish(daisyCl->ioqueue);

  // convolve Y - A.1 to B.0
  size_t convWorkerSizeDeny[2] = {daisy->paddedWidth,daisy->paddedHeight / 4};
  size_t convGroupSizeDeny[2] = {16,8};

  clSetKernelArg(daisy->oclKernels->deny, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->deny, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->deny, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->deny, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->deny, 2, 
                                 NULL, convWorkerSizeDeny, convGroupSizeDeny, 
                                 0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (deny)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  error = clFinish(daisyCl->ioqueue);

  // Gradients
  size_t gradWorkerSize = daisy->paddedWidth * daisy->paddedHeight;
  size_t gradGroupSize = 64;

  gettimeofday(&times->startGrad,NULL);

  // gradient X,Y,all - B.0 to A.0-7
  clSetKernelArg(daisy->oclKernels->grad, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->grad, 1, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->grad, 2, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->grad, 1, NULL, 
                                 &gradWorkerSize, &gradGroupSize, 0, 
                                 NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (grad)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  gettimeofday(&times->endGrad,NULL);
    
  // Smooth all to 2.5 - keep at massBuffer section A
  
  gettimeofday(&times->startConv,NULL);

  // convolve X - massBuffer sections: A to B
  size_t convWorkerSizeG0x[2] = {daisy->paddedWidth / 4, daisy->paddedHeight * daisy->gradientsNo};
  size_t convGroupSizeG0x[2] = {16,4};

  clSetKernelArg(daisy->oclKernels->G0x, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G0x, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G0x, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G0x, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G0x, 2, NULL, 
                                 convWorkerSizeG0x, convGroupSizeG0x, 0, 
                                 NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G0x)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  // convolve Y - massBuffer sections: B to A
  size_t convWorkerSizeG0y[2] = {daisy->paddedWidth, (daisy->paddedHeight * daisy->gradientsNo) / 8};
  size_t convGroupSizeG0y[2] = {16,8};

  clSetKernelArg(daisy->oclKernels->G0y, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G0y, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G0y, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G0y, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G0y, 2, NULL, 
                                 convWorkerSizeG0y, convGroupSizeG0y, 0, 
                                 NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G0y)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  // smooth all with size 23 - keep

  gettimeofday(&times->startConvX,NULL);

  // convolve X - massBuffer sections: A to C
  size_t convWorkerSizeG1x[2] = {daisy->paddedWidth / 4, daisy->paddedHeight * daisy->gradientsNo};
  size_t convGroupSizeG1x[2] = {16,4};

  clSetKernelArg(daisy->oclKernels->G1x, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G1x, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G1x, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G1x, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G1x, 2, NULL, 
                                 convWorkerSizeG1x, convGroupSizeG1x, 0, 
                                 NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G1x)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  gettimeofday(&times->endConvX,NULL);

  // convolve Y - massBuffer sections: C to B
  size_t convWorkerSizeG1y[2] = {daisy->paddedWidth, (daisy->paddedHeight * daisy->gradientsNo) / 4};
  size_t convGroupSizeG1y[2]  = {16, 16};

  clSetKernelArg(daisy->oclKernels->G1y, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G1y, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G1y, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G1y, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G1y, 2, 
                                 NULL, convWorkerSizeG1y, convGroupSizeG1y,
                                 0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G1y)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  // smooth all with size 29 - keep
  
  // convolve X - massBuffer sections: B to D
  size_t convWorkerSizeG2x[2] = {daisy->paddedWidth / 4, (daisy->paddedHeight * daisy->gradientsNo)};
  size_t convGroupSizeG2x[2]  = {16, 4};

  clSetKernelArg(daisy->oclKernels->G2x, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G2x, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G2x, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G2x, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G2x, 2, 
                                 NULL, convWorkerSizeG2x, convGroupSizeG2x, 
                                 0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G2x)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);
  
  // convolve Y - massBuffer sections: D to C
  size_t convWorkerSizeG2y[2] = {daisy->paddedWidth, (daisy->paddedHeight * daisy->gradientsNo) / 4};
  size_t convGroupSizeG2y[2]  = {16, 16};

  clSetKernelArg(daisy->oclKernels->G2y, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->G2y, 1, sizeof(filterBuffer), (void*)&filterBuffer);
  clSetKernelArg(daisy->oclKernels->G2y, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->G2y, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->G2y, 2, 
                                 NULL, convWorkerSizeG2y, convGroupSizeG2y, 
                                 0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (G2y)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  gettimeofday(&times->endConvGrad,NULL);

  gettimeofday(&times->endConv,NULL);

  // A) transpose SxGxHxW to SxHxWxG first

  gettimeofday(&times->startTransGrad,NULL);

  memorySize = daisy->gradientsNo * daisy->smoothingsNo * 
               paddedWidth * paddedHeight * sizeof(cl_float);

  cl_mem transBuffer = clCreateBuffer(daisyCl->context, CL_MEM_READ_WRITE,
                                      memorySize, (void*)NULL, &error);

  if(oclError("oclDaisy","clCreateBuffer (trans)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
  
  size_t transWorkerSize[2] = {daisy->paddedWidth,daisy->paddedHeight * daisy->smoothingsNo * daisy->gradientsNo};
  size_t transGroupSize[2] = {32,8};

  clSetKernelArg(daisy->oclKernels->trans, 0, sizeof(massBuffer), (void*)&massBuffer);
  clSetKernelArg(daisy->oclKernels->trans, 1, sizeof(transBuffer), (void*)&transBuffer);
  clSetKernelArg(daisy->oclKernels->trans, 2, sizeof(int), (void*)&(daisy->paddedWidth));
  clSetKernelArg(daisy->oclKernels->trans, 3, sizeof(int), (void*)&(daisy->paddedHeight));

  error = clEnqueueNDRangeKernel(daisyCl->ioqueue, daisy->oclKernels->trans, 2, 
                                 NULL, transWorkerSize, transGroupSize,
                                 0, NULL, NULL);

  if(oclError("oclDaisy","clEnqueueNDRangeKernel (trans)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  clFinish(daisyCl->ioqueue);

  clReleaseMemObject(massBuffer);

  gettimeofday(&times->endTransGrad,NULL);

  // B) final transposition

  gettimeofday(&times->startTransDaisy,NULL);
  
  daisy->buffers[daisy->buffersSize++] = clCreateBuffer(daisyCl->context, CL_MEM_WRITE_ONLY,
                                                        daisySectionSize,(void*)NULL, &error);

  if(oclError("oclDaisy","clCreateBuffer (daisyBufferA)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  if(totalSections > 1)
    daisy->buffers[daisy->buffersSize++] = clCreateBuffer(daisyCl->context, CL_MEM_WRITE_ONLY,
                                                          daisySectionSize,(void*)NULL, &error);

  if(oclError("oclDaisy","clCreateBuffer (daisyBufferB)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

// daisy->buffers[0] is daisyBufferA, daisy->buffers[1] is daisyBufferB
//  cl_mem daisyBufferA, daisyBufferB;

  cl_event * memoryEvents = (cl_event*)malloc(sizeof(cl_event) * totalSections);
  cl_event * kernelEvents = (cl_event*)malloc(sizeof(cl_event) * totalSections * daisy->smoothingsNo * daisy->regionPetalsNo * 2);

  error = clFinish(daisyCl->ioqueue);
  if(oclError("oclDaisy","clFinish (pre transdp)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  char * str = (char*)malloc(sizeof(char) * 200);
  int kernelsPerSection = (daisy->totalPetalsNo / 2 + 1);

  // For each 512x512 section
    // For each petal pair
  for(int sectionNo = 0; sectionNo < totalSections; sectionNo++){

    int sectionY = sectionNo;
    int sectionX = 0;

    int sectionWidth = daisyBlockWidth;
    int sectionHeight = (sectionNo < totalSections-1 ? daisyBlockHeight : 
                        (daisy->paddedHeight % daisyBlockHeight ? daisy->paddedHeight % daisyBlockHeight : daisyBlockHeight));
    int sectionSize = sectionWidth * sectionHeight * daisy->descriptorLength * sizeof(float);

    // undefined behaviour when sectionHeight is not a multiple of 16!!
    int TRANSD_FAST_STEPS = 4;
    size_t daisyWorkerSize[2] = {(sectionWidth * daisy->gradientsNo * 2) / TRANSD_FAST_STEPS, sectionHeight};
    size_t daisyGroupSize[2] = {128,1};
      
    short int resourceContext = sectionNo%2;

    cl_event * prevMemoryEvents = NULL;
    cl_event * currMemoryEvents = &memoryEvents[sectionNo];
    cl_event * prevKernelEvents = NULL;
    cl_event * currKernelEvents = &kernelEvents[sectionNo * kernelsPerSection];

    if(resourceContext != sectionNo){
      prevMemoryEvents = &memoryEvents[sectionNo-2];
      prevKernelEvents = &kernelEvents[(sectionNo-2) * kernelsPerSection];
    }

    gettimeofday(&times->startTransPinned,NULL);

    cl_mem * daisyBufferPtr = (!resourceContext ? &daisy->buffers[0] : &daisy->buffers[1]);

    //printf("Worker size %d,%d\n",daisyWorkerSize[0],daisyWorkerSize[1]);

    int kernelNo = 0;

    for(int petalNo = 1; petalNo < daisy->regionPetalsNo * daisy->smoothingsNo; petalNo+=2){

      //int petalTwoOffset = offset from region pair petal 2 from petal 1;
      //int petalOutOffset = offset from petal 1 to target descriptor in number of petals;

      int petalRegion = (petalNo-1) / daisy->regionPetalsNo;
      int petalOne = (petalNo-1) % daisy->regionPetalsNo + (petalRegion == 0);
      int petalTwo = petalOne+1;

      int petalOneY = round(allPetalOffsets[petalRegion][petalOne*2]);
      int petalOneX = round(allPetalOffsets[petalRegion][petalOne*2+1]);

      int petalTwoY = round(allPetalOffsets[petalRegion][petalTwo*2]);
      int petalTwoX = round(allPetalOffsets[petalRegion][petalTwo*2+1]);

      //int petalTwoOffset = (petalTwoY - petalOneY) * daisy->paddedWidth + (petalTwoX - petalOneX);
      //int petalTwoOffset = (petalTwoY - petalOneY) * TR_PAIRS_OFFSET_WIDTH +\
      //                     TR_PAIRS_OFFSET_WIDTH / 2 + (petalTwoX - petalOneX);
      int petalTwoOffY = (petalTwoY - petalOneY);
      int petalTwoOffX = (petalTwoX - petalOneX);

      int petalOutOffset = (-petalOneY * daisy->paddedWidth - petalOneX) * daisy->totalPetalsNo;
      petalOutOffset += (petalOutOffset < 0 ? -petalNo : petalNo);

      size_t daisyWorkerOffsets[2] = {max(0, sectionX * daisyBlockWidth + petalOneX), 
                                      petalRegion * daisy->paddedHeight + 
                                      max(0, sectionY * daisyBlockHeight + // this will cover the block borders
                                                               petalOneY)};
      
      //printf("SectionNo=%d :: PetalRegion=%d :: PetalOneY=%d :: PetalOutOffset=%d :: WorkerOffsetY=%d :: daisyBlockHeight=%d\n",sectionNo,petalRegion,petalOneY,petalOutOffset,daisyWorkerOffsets[1],daisyBlockHeight);

      clSetKernelArg(daisy->oclKernels->transdp, 0, sizeof(cl_mem), (void*)&transBuffer);
      clSetKernelArg(daisy->oclKernels->transdp, 1, sizeof(cl_mem), (void*)daisyBufferPtr);
      clSetKernelArg(daisy->oclKernels->transdp, 2, sizeof(int), (void*)&paddedWidth);
      clSetKernelArg(daisy->oclKernels->transdp, 3, sizeof(int), (void*)&paddedHeight);
      clSetKernelArg(daisy->oclKernels->transdp, 4, sizeof(int), (void*)&sectionHeight);
      clSetKernelArg(daisy->oclKernels->transdp, 5, sizeof(int), (void*)&petalTwoOffY);
      clSetKernelArg(daisy->oclKernels->transdp, 6, sizeof(int), (void*)&petalTwoOffX);
      clSetKernelArg(daisy->oclKernels->transdp, 7, sizeof(int), (void*)&petalOutOffset);

      error = clEnqueueNDRangeKernel(daisyCl->ooqueue, daisy->oclKernels->transdp, 2,
                                     daisyWorkerOffsets, daisyWorkerSize, daisyGroupSize,
                                     (resourceContext!=sectionNo), 
                                     prevMemoryEvents,
                                     &currKernelEvents[kernelNo++]);

      if(oclError("oclDaisy","clEnqueueNDRangeKernel (block pair)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
	
    }

    size_t daisyWorkerSizeSingles[2] = {daisyBlockWidth * daisy->gradientsNo, sectionHeight};
    size_t daisyWorkerOffsetsSingles[2] = {sectionX * daisyBlockWidth, sectionY * daisyBlockHeight};
    size_t daisyGroupSizeSingles[2] = {128, 1};

    clSetKernelArg(daisy->oclKernels->transds, 0, sizeof(cl_mem), (void*)&transBuffer);
    clSetKernelArg(daisy->oclKernels->transds, 1, sizeof(cl_mem), (void*)daisyBufferPtr);
    clSetKernelArg(daisy->oclKernels->transds, 2, sizeof(int), (void*)&daisyBlockHeight);

    error = clEnqueueNDRangeKernel(daisyCl->ooqueue, daisy->oclKernels->transds, 2,
                                   daisyWorkerOffsetsSingles, daisyWorkerSizeSingles, daisyGroupSizeSingles,
                                   (resourceContext!=sectionNo), prevMemoryEvents, &currKernelEvents[kernelNo++]);

      
    if(oclError("oclDaisy","clEnqueueNDRangeKernel (block single)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

    //
    // GPU->CPU transfer
    //
    if(daisy->cpuTransfer){
      int byte;

      if(sectionNo > 0){

        int descriptorsOffset = ((sectionY-1) * daisyBlockHeight * daisyBlockWidth + 
                                 sectionX * daisyBlockWidth) * daisy->descriptorLength;

        clWaitForEvents(1, currMemoryEvents-1);

        gettimeofday(&times->startTransRam,NULL);

        #pragma omp parallel for private(byte)

        for(byte = 0; byte < daisyBlockHeight * daisyBlockWidth * daisy->descriptorLength; 
                      byte += daisyBlockWidth * daisy->descriptorLength){

          memcpy(daisy->descriptors + descriptorsOffset + byte, 
                 (float*)daisyDescriptorsSection + byte, 
                 daisyBlockWidth * daisy->descriptorLength * sizeof(float));
        }
          
        gettimeofday(&times->endTransRam,NULL);

        times->transRam += timeDiff(times->startTransRam,times->endTransRam);
      }

      error = clEnqueueReadBuffer(daisyCl->ioqueue, *daisyBufferPtr, CL_FALSE,
                                  0, sectionSize, daisyDescriptorsSection,
                                  kernelsPerSection, currKernelEvents, currMemoryEvents);

      if(oclError("oclDaisy","clEnqueueReadBuffer (daisyBuffer)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

      if(sectionNo == totalSections-1 && sectionNo > 0){

        int descriptorsOffset = (sectionY * daisyBlockHeight * daisyBlockWidth + 
                                 sectionX * daisyBlockWidth) * daisy->descriptorLength;

        clWaitForEvents(1, currMemoryEvents);

        gettimeofday(&times->startTransRam,NULL);

        #pragma omp parallel for private(byte)
        for(byte = 0; byte < sectionHeight * daisyBlockWidth * daisy->descriptorLength; 
                      byte += daisyBlockWidth * daisy->descriptorLength){

          memcpy(daisy->descriptors + descriptorsOffset + byte, 
                 (float*)daisyDescriptorsSection + byte, 
                 daisyBlockWidth * daisy->descriptorLength * sizeof(float));
        }

        gettimeofday(&times->endTransRam,NULL);

        times->transRam += timeDiff(times->startTransRam,times->endTransRam);
      }
    }
    //
    // end of GPU->CPU transfer
    //
    else{

      currMemoryEvents[0] = currKernelEvents[kernelsPerSection-1];
    }

#ifdef CPU_VERIFICATION
    //
    // VERIFICATION CODE
    //

    clFinish(daisyCl->ioqueue);
    clFinish(daisyCl->ooqueue);

    float * daisyArray = (float*)malloc(paddedWidth * paddedHeight * daisy->descriptorLength * sizeof(float));
    float * transArray = (float*)malloc(paddedWidth * paddedHeight * daisy->gradientsNo * daisy->smoothingsNo * sizeof(float));

    int descriptorsOffset = (sectionY * daisyBlockHeight + sectionX) * 
                             daisyBlockWidth * daisy->descriptorLength;

    error = clEnqueueReadBuffer(daisyCl->ioqueue, *daisyBufferPtr, CL_TRUE,
                                0, sectionSize, daisyArray + descriptorsOffset,
                                0, NULL, NULL);

    error = clEnqueueReadBuffer(daisyCl->ioqueue, transBuffer, CL_TRUE,
                                0, paddedWidth * paddedHeight * daisy->gradientsNo * daisy->smoothingsNo * sizeof(float), transArray,
                                0, NULL, NULL);

    clFinish(daisyCl->ioqueue);

    long int issues = verifyTransposeDaisyPairs(daisy, transArray, daisyArray, 
                                                sectionY * daisyBlockHeight, 
                                                sectionY * daisyBlockHeight + sectionHeight-1, 
                                                allPetalOffsets);

    if(issues > 0)
      fprintf(stderr,"Got %ld issues with transposeDaisyPairs\n",issues);

    free(daisyArray);
    free(transArray);

#endif

  }

  error = clFinish(daisyCl->ioqueue);
  if(oclError("oclDaisy","clFinish io queue (end)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  error = clFinish(daisyCl->ooqueue);
  if(oclError("oclDaisy","clFinish oo queue (end)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

  gettimeofday(&times->endTransDaisy,NULL);

  times->transPinned += timeDiff(times->startTransDaisy,times->endTransDaisy) - times->transRam;

  gettimeofday(&times->endFull,NULL);

  times->difft = timeDiff(times->startFull,times->endFull);

  free(str);

#ifdef TEST_FETCHDAISY
  testFetchDaisy(daisy,daisyCl,daisy->buffers[0],times);
#endif

  //
  // GPU->CPU transfer: Release and unmap buffers, free allocated space
  //
  if(daisy->cpuTransfer){

    // don't unmap the pinned memory if there was only one block
    if(totalSections > 1){

      // Uncomment when calling oclDaisy multiple times
      error = clEnqueueUnmapMemObject(daisyCl->ioqueue, hostPinnedDaisyDescriptors, daisyDescriptorsSection, 0, NULL, NULL);

      if(oclError("oclDaisy","clEnqueueUnmapMemObject (pinned daisy)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);

      // Uncomment when calling oclDaisy multiple times
      error = clReleaseMemObject(hostPinnedDaisyDescriptors);
      if(oclError("oclDaisy","clReleaseMemObject (pinned daisy)",error)) return oclCleanUp(daisy->oclKernels,daisyCl,error);
    }

    //free(daisyDescriptorsSection);
  }
  //
  // end of GPU->CPU transfer
  //  

  clReleaseMemObject(transBuffer);
  clReleaseMemObject(filterBuffer);

  for(int s = 0; s < daisy->smoothingsNo; s++){
    clReleaseMemObject(allPairOffsetBuffers[s]);
    free(allPairOffsets[s]);
  }

  free(inputArray);

  free(filterDen);
  free(filterG0);
  free(filterG1);
  free(filterG2);
  free(memoryEvents);
  free(kernelEvents);

  return error;
}

// Generates the offsets to points in the circular petal region
// of sigma * 2 in petalsNo directions
float* generatePetalOffsets(float sigma, int petalsNo, short int firstRegion){

  if(firstRegion != 0 && firstRegion != 1) return NULL;

  float regionRadius = sigma * 2;
  float * petalOffsets = (float*)malloc(sizeof(float) * (petalsNo + firstRegion) * 2);

  if(firstRegion){
    petalOffsets[0] = 0;
    petalOffsets[1] = 0;
  }

  int i;
  for(i = firstRegion; i < petalsNo+firstRegion; i++){
    petalOffsets[i*2]   = regionRadius * sin((i-firstRegion) * (M_PI / 4));
    petalOffsets[i*2+1] = regionRadius * cos((i-firstRegion) * (M_PI / 4));
  }

  return petalOffsets;
}

// Generates pairs of neighbouring destination petal points given;
// window dimensions of local data
// offsets of a petal region
// the number of those offsets (here 8)
//
// O offset = Y * WIDTH + WIDTH/2 + X
// with; Y = [-maxOffsetY,windowHeight+maxOffsetY]  
//       X = [-maxOffsetX,windowWidth+maxOffsetX]
//       WIDTH = TR_PAIRS_OFFSET_WIDTH (special, artificially large, width to be 
//                                      able to encode negative x values in the offset)
//    decode X,Y again from O;
//    Y = floor(O / WIDTH)
//    X = O - Y * WIDTH - WIDTH/2
//
// recovery;
//    k = floor(pairedOffsets[currentPair * pairIngredients+2] / (float)TR_PAIRS_OFFSET_WIDTH);
//    l = pairedOffsets[currentPair * pairIngredients+2] - k * TR_PAIRS_OFFSET_WIDTH - TR_PAIRS_OFFSET_WIDTH/2;
//
// Current pair ratios;
// S=2.5: 812/1492 = 0.54
// S=5  : 633/1415 = 0.44
// S=7.5: 417/1631 = 0.25
//
int* generateTranspositionOffsets(int windowHeight, int windowWidth,
                                  float*  petalOffsets,
                                  int     petalsNo,
                                  int*    pairedOffsetsLength,
                                  int*    numberOfActualPairs){

  const int DEBUG = 0;

  // all offsets will be 2d array of;
  // (windowHeight+2 * maxY(petalregionoffsets)) X 
  // (windowWidth +2 * maxX(petalRegionOffsets))

  float maxOffsetY = 0;
  float maxOffsetX = 0;
  int i;
  for(i = 0; i < petalsNo; i++){
    maxOffsetY = (fabs(petalOffsets[i*2]   > maxOffsetY) ? fabs(petalOffsets[i*2])  :maxOffsetY);
    maxOffsetX = (fabs(petalOffsets[i*2+1] > maxOffsetX) ? fabs(petalOffsets[i*2+1]):maxOffsetX);
  }
  maxOffsetY = ceil(fabs(maxOffsetY));
  maxOffsetX = ceil(fabs(maxOffsetX));

  int offsetsHeight = windowHeight + 2 * maxOffsetY;
  int offsetsWidth  = windowWidth  + 2 * maxOffsetX;

  int * allSources = (int*)malloc(sizeof(int) * offsetsHeight * offsetsWidth
                                              * petalsNo * 2);

  const int noSource = -999;

  // generate em
  int x,y,j;
  j = 0;
  for(y = -maxOffsetY; y < windowHeight+maxOffsetY; y++){
    for(x = -maxOffsetX; x < windowWidth+maxOffsetX; x++){
      int fromY,fromX;
      for(i = 0; i < petalsNo; i++,j++){
        fromY = round(y+petalOffsets[i*2]);
        fromX = round(x+petalOffsets[i*2+1]);
        if(fromY >= 0 && fromY < windowHeight && fromX >= 0 && fromX < windowWidth){
          allSources[j*2] = fromY;
          allSources[j*2+1] = fromX;
        }
        else{
          allSources[j*2] = noSource;
          allSources[j*2+1] = noSource;
        }
        if(DEBUG){
          if(y == 0 && x == 0)
            printf("Offsets at %d,%d,%d: (%d,%d)\n",y,x,i,allSources[j*2],allSources[j*2+1]);
        }
      }
    }
  }

  int pairIngredients = 4;
  int * pairedOffsets = (int*)malloc(sizeof(int) * offsetsHeight * offsetsWidth
                                                 * petalsNo * pairIngredients);

  // pair em up
  const int isLeftOver = 1;

  char * singlesLeftOver = (char*)malloc(sizeof(char) * offsetsHeight * offsetsWidth * petalsNo);

  for(i = 0; i < offsetsHeight * offsetsWidth * petalsNo; i++)
    singlesLeftOver[i] = !isLeftOver; // initialise value

  int currentPair = 0;
  int thisSourceY,thisSourceX;
  int nextSourceY,nextSourceX;
  j = 0;
  for(y = 0; y < offsetsHeight; y++){
    for(i = 0; i < offsetsWidth*petalsNo-1; i++){
      j = y*offsetsWidth*petalsNo+i;
      x = i / petalsNo;


      thisSourceY = allSources[j*2];
      thisSourceX = allSources[j*2+1];
      nextSourceY = allSources[j*2+2];
      nextSourceX = allSources[j*2+3];

      if(i % petalsNo == petalsNo-1){ // can't pair up last with first, the sets of values in the destination array are not continuous
        if(thisSourceY != noSource)
          singlesLeftOver[j] = isLeftOver;
        continue;
      }
      if(DEBUG){
        if(y == 15)
          printf("(Y %d,X %d): (sourceY,sourceX),(nextY,nextX) = (%d,%d),(%d,%d)\n",y,x,thisSourceY,thisSourceX,nextSourceY,nextSourceX);
      }
      if(thisSourceY != noSource && nextSourceY != noSource){
        pairedOffsets[currentPair * pairIngredients]   = thisSourceY * windowWidth + thisSourceX; // p1
        pairedOffsets[currentPair * pairIngredients+1] = nextSourceY * windowWidth + nextSourceX; // p2
        pairedOffsets[currentPair * pairIngredients+2] = (y-maxOffsetY) * TR_PAIRS_OFFSET_WIDTH +\
                                                         TR_PAIRS_OFFSET_WIDTH/2 + (x-maxOffsetX); // o, special 1D offset in image coordinates
        pairedOffsets[currentPair * pairIngredients+3] = i % petalsNo; // petal
        if(DEBUG){
          if(y == 15)
            printf("Pair %d: (p1,p2,o,petal) = (%d,%d,%d,%d)\n",currentPair,pairedOffsets[currentPair * pairIngredients],
                                                                            pairedOffsets[currentPair * pairIngredients+1],
                                                                            pairedOffsets[currentPair * pairIngredients+2],
                                                                            pairedOffsets[currentPair * pairIngredients+3]);
        }
        currentPair++;
        i++;
      }
      else if(thisSourceY != noSource && nextSourceY == noSource){
        singlesLeftOver[j] = isLeftOver;
      }
    }
    if(i == offsetsWidth*petalsNo-1){ // ie if the last data was not paired up
      j = y*offsetsWidth*petalsNo+i-1;
      if(allSources[j*2] != noSource)
        singlesLeftOver[j] = isLeftOver;
    }
  }
  
  *numberOfActualPairs = currentPair;

  for(j = 0; j < offsetsHeight*offsetsWidth*petalsNo; j++){

    if(singlesLeftOver[j] == isLeftOver){
      y = j / (offsetsWidth * petalsNo);
      x = (j % (offsetsWidth * petalsNo)) / petalsNo;
      thisSourceY = allSources[j*2];
      thisSourceX = allSources[j*2+1];
      pairedOffsets[currentPair * pairIngredients] = thisSourceY * windowWidth + thisSourceX; // p1
      pairedOffsets[currentPair * pairIngredients+1] = TR_PAIRS_SINGLE_ONLY;
      pairedOffsets[currentPair * pairIngredients+2] = (y-maxOffsetY) * TR_PAIRS_OFFSET_WIDTH +\
                                                       TR_PAIRS_OFFSET_WIDTH/2 + (x-maxOffsetX); // o, the special 1d offset
      pairedOffsets[currentPair * pairIngredients+3] = j % petalsNo;
      if(DEBUG){
        if(x == 15){
          printf("Single %d (%.0f,%.0f): (p1,p2,o,petal) = (%d,%d,%d,%d)\n",currentPair,y-maxOffsetY,x-maxOffsetX,
                                                                        pairedOffsets[currentPair * pairIngredients],
                                                                        pairedOffsets[currentPair * pairIngredients+1],
                                                                        pairedOffsets[currentPair * pairIngredients+2],
                                                                        pairedOffsets[currentPair * pairIngredients+3]);
          int k,l;
          k = floor(pairedOffsets[currentPair * pairIngredients+2] / (float)TR_PAIRS_OFFSET_WIDTH);
          l = pairedOffsets[currentPair * pairIngredients+2] - k * TR_PAIRS_OFFSET_WIDTH - TR_PAIRS_OFFSET_WIDTH/2;
          printf("Recovered Y,X = %d,%d\n",k,l);
        }
      }
      currentPair++;
    }
  }

  free(allSources);
  free(singlesLeftOver);

  *pairedOffsetsLength = currentPair;

  return pairedOffsets;
}

void testFetchDaisy(daisy_params * daisy, ocl_constructs * daisyCl, cl_mem daisyBufferA, time_params * times){
  //
  // Test DAISY fetch speed
  //

  int rangeStart = FETCH_RANGE_START;
  int rangeEnd = FETCH_RANGE_START;
  int step = 512;

  int rangeLength = ((rangeEnd - rangeStart) / step + 2);
  int iterations = 1;

  double * fetchTimes = (double*)malloc(sizeof(double) * iterations);

  size_t fetchGroupSize[1] = {256};
  size_t fetchWorkerSize[1] = {fetchGroupSize[0]};
  size_t fetchWorkerOffsets[1] = {0};

  for(int i = 0; i < iterations; i++){
    // measure for 1 descriptor
    fetchWorkerSize[0] = fetchGroupSize[0];

    clSetKernelArg(daisy->oclKernels->fetchd, 0, sizeof(cl_mem), (void*)&daisyBufferA);
    gettimeofday(&times->startFetchDaisy,NULL);
    clEnqueueNDRangeKernel(daisyCl->ooqueue, daisy->oclKernels->fetchd, 1,
                           fetchWorkerOffsets, fetchWorkerSize, fetchGroupSize,
                           0, NULL, NULL);

    clFinish(daisyCl->ooqueue);
    gettimeofday(&times->endFetchDaisy,NULL);
    fetchTimes[0] += timeDiff(times->startFetchDaisy,times->endFetchDaisy);

    // measure for rangeStart-rangeEnd
    for(int r = 1; r < rangeLength; r++){

      int descriptors = rangeStart + (r-1) * step;

      fetchWorkerSize[0] = fetchGroupSize[0] * descriptors;

      clSetKernelArg(daisy->oclKernels->fetchd, 0, sizeof(cl_mem), (void*)&daisyBufferA);

      gettimeofday(&times->startFetchDaisy,NULL);

      clEnqueueNDRangeKernel(daisyCl->ooqueue, daisy->oclKernels->fetchd, 1,
                             fetchWorkerOffsets, fetchWorkerSize, fetchGroupSize,
                             0, NULL, NULL);

      clFinish(daisyCl->ooqueue);
      gettimeofday(&times->endFetchDaisy,NULL);
      fetchTimes[r] += timeDiff(times->startFetchDaisy,times->endFetchDaisy);
    }
    printf("end\n");
  }

  const char * csvOutName = "gdaisy-speeds-FETCHWRITEDAISY.csv";
  FILE * csvOut = fopen(csvOutName,"w");
  fprintf(csvOut,"descriptors,msec\n");
  fprintf(csvOut,"1,%.6f\n",fetchTimes[0] / iterations);
  for(int r = 1; r < rangeLength; r++){
    fprintf(csvOut,"%d,%.6f\n",rangeStart+(r-1)*step,(fetchTimes[r])/iterations);
  }
  fclose(csvOut);
  printf("Fetch daisy results written to %s.\n",csvOutName);
  //
  // end of DAISY fetch
  //
}

long int verifyTransposeDaisyPairs(daisy_params * daisy, float * transArray, float * daisyArray, int startRow, int endRow, float ** allPetalOffsets){

  long int issues = 0;

  long int petalIssues = 0;
  printf("test seg 01\n");

  printf("Petal 1: (0,0)\n");
  for(int petalNo = 0; petalNo < 1; petalNo++){


    for(int y = startRow; y < endRow+1; y++){

      for(int x = 0; x < daisy->paddedWidth; x++){

        float cpuVal = transArray[(y * daisy->paddedWidth + x)* daisy->gradientsNo];
        float gpuVal = daisyArray[((y * daisy->paddedWidth + x) * (daisy->totalPetalsNo + TRANSD_FAST_PETAL_PADDING) + petalNo + TRANSD_FAST_PETAL_PADDING) * daisy->gradientsNo];

        if(abs(cpuVal - gpuVal) > 0.01){
          if(petalIssues++ < 10){
            fprintf(stderr,"daisyTransSingles issue: from (%d,%d) to (%d,%d,%d) - [CPU=%.3f,GPU=%.3f]\n",y,x,y,x,petalNo,cpuVal,gpuVal);
          }
        }
      }

    }

  }

  printf("Petal issues: %ld\n",petalIssues);

  for(int petalNo = 1; petalNo < daisy->regionPetalsNo * daisy->smoothingsNo; petalNo+=2){
    petalIssues = 0;
//    int petalTwoOffset = offset from region pair petal 2 from petal 1;
//    int petalOutOffset = offset from petal 1 to target descriptor in number of petals;

    int petalRegion = (petalNo-1) / daisy->regionPetalsNo;
    int petalOne = (petalNo-1) % daisy->regionPetalsNo + (petalRegion == 0);
    int petalTwo = petalOne+1;

    int petalOneY = round(allPetalOffsets[petalRegion][petalOne*2]);
    int petalOneX = round(allPetalOffsets[petalRegion][petalOne*2+1]);

    int petalTwoY = round(allPetalOffsets[petalRegion][petalTwo*2]);
    int petalTwoX = round(allPetalOffsets[petalRegion][petalTwo*2+1]);

    printf("Petals: 1 = (%d,%d), 2 = (%d,%d)\n",petalOneY,petalOneX,petalTwoY,petalTwoX);

//    int petalTwoOffset = (petalTwoY - petalOneY) * daisy->paddedWidth + (petalTwoX - petalOneX);
//        pairedOffsets[currentPair * pairIngredients+2] = (y-maxOffsetY) * TR_PAIRS_OFFSET_WIDTH +\
//                                                         TR_PAIRS_OFFSET_WIDTH/2 + (x-maxOffsetX); // o, special 1D offset in image coordinates
    int petalTwoOffset = (petalTwoY - petalOneY) * TR_PAIRS_OFFSET_WIDTH +\
                         TR_PAIRS_OFFSET_WIDTH / 2 + (petalTwoX - petalOneX);

    int petalOutOffset = (-petalOneY * daisy->paddedWidth - petalOneX) * daisy->totalPetalsNo;
    petalOutOffset += (petalOutOffset < 0 ? -petalNo : petalNo);

    for(int y = petalRegion * daisy->paddedHeight + startRow; y < petalRegion * daisy->paddedHeight + endRow + 1; y++){
      
      for(int x = 0; x < daisy->paddedWidth; x++){

//    k = floor(pairedOffsets[currentPair * pairIngredients+2] / (float)TR_PAIRS_OFFSET_WIDTH);
//    l = pairedOffsets[currentPair * pairIngredients+2] - k * TR_PAIRS_OFFSET_WIDTH - TR_PAIRS_OFFSET_WIDTH/2;
        int petalTwoOffsetY = floor(petalTwoOffset / (float) TR_PAIRS_OFFSET_WIDTH);
        int petalTwoOffsetX = petalTwoOffset - petalTwoOffsetY * TR_PAIRS_OFFSET_WIDTH - TR_PAIRS_OFFSET_WIDTH / 2;

        if(petalTwoOffsetY != (petalTwoY-petalOneY) || petalTwoOffsetX != (petalTwoX-petalOneX)){
          fprintf(stderr,"petalTwoOffset alarm in YX!!!\n");
          fprintf(stderr,"Input YX (%d,%d) gone to %d and output YX (%d,%d)\n",(petalTwoY-petalOneY),(petalTwoX-petalOneX),petalTwoOffset,petalTwoOffsetY,petalTwoOffsetX);
        }
        if((petalOutOffset / daisy->totalPetalsNo) / daisy->paddedWidth != -petalOneY)
          fprintf(stderr,"petalOutOffset alarm in Y!!!\n");
        if((petalOutOffset / daisy->totalPetalsNo) % daisy->paddedWidth != -petalOneX)
          fprintf(stderr,"petalOutOffset alarm in X!!!\n");
        if(abs(petalOutOffset % daisy->totalPetalsNo) != petalNo)
          fprintf(stderr,"petalOutOffset alarm in PETAL!!!\n");

        int targetY = y % daisy->paddedHeight + (petalOutOffset / daisy->totalPetalsNo) / daisy->paddedWidth;
        int targetX = x + (petalOutOffset / daisy->totalPetalsNo) % daisy->paddedWidth;

        if(targetY < startRow || targetY > endRow || targetX < 0 || targetX >= daisy->paddedWidth) continue;

        float cpuVal = .0f;
        float gpuVal = .0f;

        cpuVal += transArray[(y * daisy->paddedWidth + x) * daisy->gradientsNo];
        gpuVal += daisyArray[((targetY * daisy->paddedWidth + targetX) * (daisy->totalPetalsNo + TRANSD_FAST_PETAL_PADDING) + abs(petalOutOffset % daisy->totalPetalsNo) + TRANSD_FAST_PETAL_PADDING) * daisy->gradientsNo];

        if(fabs(gpuVal - cpuVal) > 0.01){
          petalIssues++;
          if(issues++ < 10){
            printf("PetalOutOffset %d\n",petalOutOffset);
            fprintf(stderr,"daisyTransPairs issue (1): from (%d,%d) to (%d,%d,%d) - [CPU=%.3f,GPU=%.3f]\n",y,x,targetY,targetX,abs(petalOutOffset % daisy->totalPetalsNo),cpuVal,gpuVal);
          }
        }

        cpuVal = .0f;
        gpuVal = .0f;
        int y2 = y % daisy->paddedHeight + petalTwoOffsetY;
        int x2 = x + petalTwoOffsetX;
        if(y2 < 0 || y2 >= daisy->paddedHeight || x2 < 0 || x2 >= daisy->paddedWidth){}
        else{ 
          y2 = y + petalTwoOffsetY;
          cpuVal += transArray[(y2 * daisy->paddedWidth + x2) * daisy->gradientsNo];
        }

        gpuVal += daisyArray[((targetY * daisy->paddedWidth + targetX) * (daisy->totalPetalsNo + TRANSD_FAST_PETAL_PADDING) + abs(petalOutOffset % daisy->totalPetalsNo) + 1 + TRANSD_FAST_PETAL_PADDING) * daisy->gradientsNo];

        if(fabs(gpuVal - cpuVal) > 0.01){
          petalIssues++;
          if(issues++ < 10){
            fprintf(stderr,"daisyTransPairs issue (2): from (%d,%d) to (%d,%d,%d) - [CPU=%.3f,GPU=%.3f]\n",y,x,targetY,targetX,abs(petalOutOffset % daisy->totalPetalsNo)+1,cpuVal,gpuVal);
          }
        }

      }

    }

    printf("Petal issues: %ld\n",petalIssues);

  }

  return issues;

}

// returns milliseconds
double timeDiff(struct timeval start, struct timeval end){

  return (end.tv_sec*1000.0+(end.tv_usec/1000.0)) - (start.tv_sec*1000.0+(start.tv_usec/1000.0));

}

/*

void unused_old_kernel_verification_code(){

  //
  // VERIFICATION CODE
  //

  int issues = 0;

  // Verify transposition b)
  int TESTING_TRANSD = times->measureDeviceHostTransfers;
  if(DEBUG_ALL && TESTING_TRANSD){


    for(int block = 0; block < totalSections; block++){

      int sectionY = block;
      int sectionX = 0;

      int sectionWidth = daisyBlockWidth;
      int sectionHeight = (block < totalSections-1 ? daisyBlockHeight : 
                          (daisy->paddedHeight % daisyBlockHeight ? daisy->paddedHeight % daisyBlockHeight : daisyBlockHeight));

      for(int smoothingNo = 0; smoothingNo < daisy->smoothingsNo; smoothingNo++){

        int pairOffsetsLength = allPairOffsetsLengths[smoothingNo];
        int * pairOffsets = allPairOffsets[smoothingNo];

        int petalsNo = daisy->petalsNo + (smoothingNo==0);

        int petalStart = (smoothingNo > 0 ? smoothingNo * daisy->petalsNo + 1 : 0);

        //printf("\nPetalsNo = %d\n",petalsNo);
        float * daisyArray = daisy->descriptors + block * daisyBlockHeight * daisyBlockWidth * 200;

        clFinish(daisyCl->ioqueue);

        error = clEnqueueReadBuffer(daisyCl->ioqueue, transBuffer, CL_TRUE,
                                    paddedWidth * paddedHeight * 8 * smoothingNo * sizeof(float), 
                                    paddedWidth * paddedHeight * 8 * sizeof(float), inputArray,
                                    0, NULL, NULL);

        clFinish(daisyCl->ioqueue);

        short int issued = 0;
  
        int topLeftY = 16;
        int topLeftX = 16;
        int yStep = 16;
        int xStep = 16;
        int y,x;
        int yBlockOffset = sectionY * daisyBlockHeight;
        int xBlockOffset = sectionX * daisyBlockWidth;
        for(y = topLeftY; y < sectionHeight-topLeftY; y+=yStep){
          for(x = topLeftX; x < sectionWidth-topLeftX; x+=xStep){
            //printf("Testing yx %d,%d\n",y,x);
            int p;
            for(p = 0; p < pairOffsetsLength; p++){
              int src1 = (yBlockOffset + pairOffsets[p * 4] / 16 + y) * paddedWidth * 8 + (xBlockOffset + pairOffsets[p * 4] % 16 + x) * 8;
              int src2 = (yBlockOffset + pairOffsets[p * 4+1] / 16 + y) * paddedWidth * 8 + (xBlockOffset + pairOffsets[p * 4+1] % 16 + x) * 8;
              int dst  = floor(pairOffsets[p * 4+2] / 1000.0f);
              int petal = pairOffsets[p * 4+3];
              dst = (dst + y) * daisyBlockWidth * 200 + (pairOffsets[p * 4+2] - dst * 1000 - 500 + x) * 200 + (petalStart+petal) * 8;
              int j;
              for(j = 0; j < 8; j++){
                if(fabs(daisyArray[dst+j] - inputArray[src1+j]) > 0.00001){
                  if(!issued)
                    printf("Issue at section %d,%d S=%d(1)\n",sectionY,sectionX,smoothingNo);
                  //printf("P%d - Issue at (1)%d,%d\n",p,y,x);
                  issued=1;
                  issues++;
                }
              }
              if(pairOffsets[p * 4+1] != TR_PAIRS_SINGLE_ONLY){
                for(j = 8; j < 16; j++){
                  if(fabs(daisyArray[dst+j] - inputArray[src2+j%8]) > 0.00001){
                    if(!issued)
                      printf("Issues at section %d,%d S=%d(2)\n",sectionY,sectionX,smoothingNo);
                    //printf("P%d - Issue at (2)%d,%d\n",p,y,x);
                    issued=1;
                  }
                }
              }
            }
          }
        }
      }
    }
    printf("%d issues\n",issues);
  }

  //
  // END OF VERIFICATION CODE
  //

}

*/

