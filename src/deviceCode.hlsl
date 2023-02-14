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

#include "sharedCode.h"

struct Payload {
  float3 color;
};

float4 over(float4 a, float4 b) {
  float4 result;
  result.a = a.a + b.a * (1.f - a.a);
  result.rgb = (a.rgb * a.a + b.rgb * b.a * (1.f - a.a)) / result.a;
  return result;
}

GPRT_RAYGEN_PROGRAM(simpleRayGen, (RayGenData, record)) {
  Payload payload;
  uint2 pixelID = DispatchRaysIndex().xy;
  uint2 fbSize = DispatchRaysDimensions().xy;
  float2 screen = (float2(pixelID) + float2(.5f, .5f)) / float2(fbSize);

  RayDesc rayDesc;
  rayDesc.Origin = record.camera.pos;
  rayDesc.Direction =
      normalize(record.camera.dir_00 + screen.x * record.camera.dir_du + screen.y * record.camera.dir_dv);
  rayDesc.TMin = 0.0;
  rayDesc.TMax = 10000.0;
  RaytracingAccelerationStructure world = gprt::getAccelHandle(record.world);
  TraceRay(world,                   // the tree
           RAY_FLAG_NONE,           // ray flags
           0xff,                    // instance inclusion mask
           0,                       // ray type
           1,                       // number of ray types
           0,                       // miss type
           rayDesc,                 // the ray to trace
           payload                  // the payload IO
  );

  // Composite on top of everything else our user interface
  Texture2D guiTexture = gprt::getTexture2DHandle(record.guiTexture);
  SamplerState guiSampler = gprt::getDefaultSampler();
  float4 guiColor = guiTexture.SampleGrad(guiSampler, screen, float2(0.f, 0.f), float2(0.f, 0.f));
  float4 finalColor = over(float4(pow(guiColor.rgb, 2.2f), guiColor.w), float4(payload.color.r, payload.color.g, payload.color.b, 1.f));

  const int fbOfs = pixelID.x + fbSize.x * pixelID.y;
  gprt::store(record.frameBuffer, fbOfs, gprt::make_bgra(finalColor));
}

struct Attribute {
  float radius;
  float3 position;
};

GPRT_ANY_HIT_PROGRAM(SphereAnyHit, (SphereGeomData, record), (Payload, payload), (Attribute, attribute)) {
  float3 origin = attribute.position;
  float3 hitPos = ObjectRayOrigin() + RayTCurrent() * ObjectRayDirection();
  float3 normal = normalize(hitPos - origin);
  payload.color = abs(normal);

  if (record.terminateEarly == 1) {
    AcceptHitAndEndSearch(); // This causes the payload color write above to break
  }
}

GPRT_COMPUTE_PROGRAM(SphereBounds, (SphereBoundsData, record), (1, 1, 1)) {
  int primID = DispatchThreadID.x;
  float3 position = gprt::load<float3>(record.vertex, primID);
  float radius = gprt::load<float>(record.radius, primID);
  float3 aabbMin = position - float3(radius, radius, radius);
  float3 aabbMax = position + float3(radius, radius, radius);
  gprt::store(record.aabbs, 2 * primID, aabbMin);
  gprt::store(record.aabbs, 2 * primID + 1, aabbMax);
}

GPRT_INTERSECTION_PROGRAM(SphereIntersection, (SphereGeomData, record)) {
  uint primID = PrimitiveIndex();

  float3 position = gprt::load<float3>(record.vertex, primID);
  float radius = gprt::load<float>(record.radius, primID);

  float3 ro = ObjectRayOrigin();
  float3 rd = ObjectRayDirection();

  float3 oc = ro - position;
  float b = dot(oc, rd);
  float c = dot(oc, oc) - radius * radius;
  float h = b * b - c;

  if (h < 0.0)
    return;
  float tHit = -b - sqrt(h);

  Attribute attr;
  attr.radius = radius;
  attr.position = position;
  ReportHit(tHit, /*hitKind*/ 0, attr);
}

GPRT_MISS_PROGRAM(miss, (MissProgData, record), (Payload, payload)) {
  uint2 pixelID = DispatchRaysIndex().xy;
  int pattern = (pixelID.x / 32) ^ (pixelID.y / 32);
  payload.color = (pattern & 1) ? record.color1 : record.color0;
}
