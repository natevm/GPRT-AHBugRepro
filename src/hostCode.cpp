// MIT License

// Copyright (c) 2022 Nathan V. Morrical

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// This program sets up a single geometric object, a mesh for a cube, and
// its acceleration structure, then ray traces it.

// public GPRT API
#include <gprt.h>

// our shared data structures between host and device
#include "sharedCode.h"

#include "imgui.h"

#define LOG(message)                                                                                                   \
  std::cout << GPRT_TERMINAL_BLUE;                                                                                     \
  std::cout << "#gprt.sample(main): " << message << std::endl;                                                         \
  std::cout << GPRT_TERMINAL_DEFAULT;
#define LOG_OK(message)                                                                                                \
  std::cout << GPRT_TERMINAL_LIGHT_BLUE;                                                                               \
  std::cout << "#gprt.sample(main): " << message << std::endl;                                                         \
  std::cout << GPRT_TERMINAL_DEFAULT;

extern GPRTProgram sample_deviceCode;

// Vertices and radii that will be used to define spheres
const int NUM_VERTICES = 11;
float3 vertices[NUM_VERTICES] = {
    {0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f},
    {0.4f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.6f, 0.0f, 0.0f}, {0.7f, 0.0f, 0.0f},
    {0.8f, 0.0f, 0.0f}, {0.9f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
};

float radii[NUM_VERTICES] = {.015f, .025f, .035f, .045f, .055f, .065f, .055f, .045f, .035f, .025f, .015f};

// initial image resolution
const int2 fbSize = {1400, 460};

// final image output
const char *outFileName = "sample_program.png";

// Initial camera parameters
float3 lookFrom = {0.5f, 0.0f, 0.6f};
float3 lookAt = {0.5f, 0.f, 0.f};
float3 lookUp = {0.f, -1.f, 0.f};
float cosFovy = 0.66f;

#include <iostream>
int
main(int ac, char **av) {
  // create a context on the first device:
  gprtRequestWindow(fbSize.x, fbSize.y, "Sample Program");
  GPRTContext context = gprtContextCreate(nullptr, 1);
  GPRTModule module = gprtModuleCreate(context, sample_deviceCode);

  // ##################################################################
  // set up all the GPU kernels we want to run
  // ##################################################################

  // -------------------------------------------------------
  // declare geometry type
  // -------------------------------------------------------
  GPRTGeomTypeOf<SphereGeomData> sphereGeomType = gprtGeomTypeCreate<SphereGeomData>(context, GPRT_AABBS);
  gprtGeomTypeSetAnyHitProg(sphereGeomType, 0, module, "SphereAnyHit");
  gprtGeomTypeSetIntersectionProg(sphereGeomType, 0, module, "SphereIntersection");


  // -------------------------------------------------------
  // set up sphere bounding box compute program
  // -------------------------------------------------------
  GPRTComputeOf<SphereBoundsData> boundsProgram = gprtComputeCreate<SphereBoundsData>(context, module, "SphereBounds");

  // -------------------------------------------------------
  // set up miss
  // -------------------------------------------------------
  GPRTMissOf<MissProgData> miss = gprtMissCreate<MissProgData>(context, module, "miss");

  // -------------------------------------------------------
  // set up ray gen program
  // -------------------------------------------------------
  GPRTRayGenOf<RayGenData> rayGen = gprtRayGenCreate<RayGenData>(context, module, "simpleRayGen");

  // ##################################################################
  // set the parameters for our compute kernel
  // ##################################################################

  // ------------------------------------------------------------------
  // aabb mesh
  // ------------------------------------------------------------------
  GPRTBufferOf<float3> vertexBuffer = gprtDeviceBufferCreate<float3>(context, NUM_VERTICES, vertices);
  GPRTBufferOf<float> radiusBuffer = gprtDeviceBufferCreate<float>(context, NUM_VERTICES, radii);
  GPRTBufferOf<float3> aabbPositionsBuffer = gprtDeviceBufferCreate<float3>(context, NUM_VERTICES * 2, nullptr);

  GPRTGeomOf<SphereGeomData> aabbGeom = gprtGeomCreate(context, sphereGeomType);
  gprtAABBsSetPositions(aabbGeom, aabbPositionsBuffer, NUM_VERTICES);

  SphereGeomData *geomData = gprtGeomGetParameters(aabbGeom);
  geomData->vertex = gprtBufferGetHandle(vertexBuffer);
  geomData->radius = gprtBufferGetHandle(radiusBuffer);

  // to reproduce the bug
  geomData->terminateEarly = false;

  SphereBoundsData *boundsData = gprtComputeGetParameters(boundsProgram);
  boundsData->vertex = gprtBufferGetHandle(vertexBuffer);
  boundsData->radius = gprtBufferGetHandle(radiusBuffer);
  boundsData->aabbs = gprtBufferGetHandle(aabbPositionsBuffer);

  // compute AABBs in parallel with a compute shader
  gprtBuildShaderBindingTable(context, GPRT_SBT_COMPUTE);

  // Launch the compute kernel, which will populate our aabbPositionsBuffer
  gprtComputeLaunch1D(context, boundsProgram, NUM_VERTICES);

  // Now that the aabbPositionsBuffer is filled, we can compute our AABB
  // acceleration structure
  GPRTAccel aabbAccel = gprtAABBAccelCreate(context, 1, &aabbGeom);
  gprtAccelBuild(context, aabbAccel);

  GPRTAccel world = gprtInstanceAccelCreate(context, 1, &aabbAccel);
  gprtAccelBuild(context, world);

  // ##################################################################
  // set the parameters for the rest of our kernels
  // ##################################################################

  // Setup pixel frame buffer
  GPRTBuffer frameBuffer = gprtDeviceBufferCreate(context, sizeof(uint32_t), fbSize.x * fbSize.y);

  auto guiColorAttachment = gprtDeviceTextureCreate<uint32_t>(
      context, GPRT_IMAGE_TYPE_2D, GPRT_FORMAT_R8G8B8A8_SRGB, fbSize.x, fbSize.y, 1, false, nullptr);
  auto guiDepthAttachment = gprtDeviceTextureCreate<float>(
      context, GPRT_IMAGE_TYPE_2D, GPRT_FORMAT_D32_SFLOAT, fbSize.x, fbSize.y, 1, false, nullptr);
  gprtGuiSetRasterAttachments(context, guiColorAttachment, guiDepthAttachment);


  // Raygen program frame buffer
  RayGenData *rayGenData = gprtRayGenGetParameters(rayGen);
  rayGenData->frameBuffer = gprtBufferGetHandle(frameBuffer);
  rayGenData->guiTexture = gprtTextureGetHandle(guiColorAttachment);
  rayGenData->world = gprtAccelGetHandle(world);

  // Miss program checkerboard background colors
  MissProgData *missData = gprtMissGetParameters(miss);
  missData->color0 = float3(0.1f, 0.1f, 0.1f);
  missData->color1 = float3(0.0f, 0.0f, 0.0f);

  gprtBuildShaderBindingTable(context, GPRT_SBT_ALL);

  // ##################################################################
  // now that everything is ready: launch it ....
  // ##################################################################

  LOG("launching ...");

  bool firstFrame = true;
  double xpos = 0.f, ypos = 0.f;
  double lastxpos, lastypos;
  do {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::NewFrame();

    float speed = .001f;
    lastxpos = xpos;
    lastypos = ypos;
    gprtGetCursorPos(context, &xpos, &ypos);
    if (firstFrame) {
      lastxpos = xpos;
      lastypos = ypos;
    }
    int state = gprtGetMouseButton(context, GPRT_MOUSE_BUTTON_LEFT);

    // If we click the mouse, we should rotate the camera
    // Here, we implement some simple camera controls
    if (state == GPRT_PRESS && !io.WantCaptureMouse || firstFrame) {
      firstFrame = false;
      float4 position = {lookFrom.x, lookFrom.y, lookFrom.z, 1.f};
      float4 pivot = {lookAt.x, lookAt.y, lookAt.z, 1.0};
#ifndef M_PI
#define M_PI 3.1415926f
#endif

      // step 1 : Calculate the amount of rotation given the mouse movement.
      float deltaAngleX = (2 * M_PI / fbSize.x);
      float deltaAngleY = (M_PI / fbSize.y);
      float xAngle = (lastxpos - xpos) * deltaAngleX;
      float yAngle = (lastypos - ypos) * deltaAngleY;

      // step 2: Rotate the camera around the pivot point on the first axis.
      float4x4 rotationMatrixX = rotation_matrix(rotation_quat(lookUp, xAngle));
      position = (mul(rotationMatrixX, (position - pivot))) + pivot;

      // step 3: Rotate the camera around the pivot point on the second axis.
      float3 lookRight = cross(lookUp, normalize(pivot - position).xyz());
      float4x4 rotationMatrixY = rotation_matrix(rotation_quat(lookRight, yAngle));
      lookFrom = ((mul(rotationMatrixY, (position - pivot))) + pivot).xyz();

      // ----------- compute variable values  ------------------
      float3 camera_pos = lookFrom;
      float3 camera_d00 = normalize(lookAt - lookFrom);
      float aspect = float(fbSize.x) / float(fbSize.y);
      float3 camera_ddu = cosFovy * aspect * normalize(cross(camera_d00, lookUp));
      float3 camera_ddv = cosFovy * normalize(cross(camera_ddu, camera_d00));
      camera_d00 -= 0.5f * camera_ddu;
      camera_d00 -= 0.5f * camera_ddv;

      // ----------- set variables  ----------------------------
      RayGenData *raygenData = gprtRayGenGetParameters(rayGen);
      raygenData->camera.pos = camera_pos;
      raygenData->camera.dir_00 = camera_d00;
      raygenData->camera.dir_du = camera_ddu;
      raygenData->camera.dir_dv = camera_ddv;
    }

    static int choice = 0;
    ImGui::RadioButton("Don't terminate traversal early?", &choice, 0);
    ImGui::RadioButton("Terminate Traversal Early?", &choice, 1);
    SphereGeomData *geomData = gprtGeomGetParameters(aabbGeom);
    geomData->terminateEarly = choice;
    ImGui::EndFrame();
    
    gprtBuildShaderBindingTable(context); 

    gprtTextureClear(guiDepthAttachment);
    gprtTextureClear(guiColorAttachment);
    gprtGuiRasterize(context);

    // Calls the GPU raygen kernel function
    gprtRayGenLaunch2D(context, rayGen, fbSize.x, fbSize.y);

    // If a window exists, presents the framebuffer here to that window
    gprtBufferPresent(context, frameBuffer);
  }
  // returns true if "X" pressed or if in "headless" mode
  while (!gprtWindowShouldClose(context));

  // Save final frame to an image
  gprtBufferSaveImage(frameBuffer, fbSize.x, fbSize.y, outFileName);

  // ##################################################################
  // and finally, clean up
  // ##################################################################

  gprtBufferDestroy(vertexBuffer);
  gprtBufferDestroy(radiusBuffer);
  gprtBufferDestroy(aabbPositionsBuffer);
  gprtBufferDestroy(frameBuffer);
  gprtRayGenDestroy(rayGen);
  gprtMissDestroy(miss);
  gprtComputeDestroy(boundsProgram);
  gprtAccelDestroy(aabbAccel);
  gprtAccelDestroy(world);
  gprtGeomDestroy(aabbGeom);
  gprtGeomTypeDestroy(sphereGeomType);
  gprtModuleDestroy(module);
  gprtContextDestroy(context);
}
