// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "VulkanDevice.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "HaltonSequence.h"
#include "Matrix.h"
#include "RenderResolutionHelper.h"
#include "RgException.h"
#include "Utils.h"
#include "Generated/ShaderCommonC.h"

VkCommandBuffer RTGL1::VulkanDevice::BeginFrame(const RgStartFrameInfo &startInfo)
{
    uint32_t frameIndex = currentFrameState.IncrementFrameIndexAndGet();

    if (!waitForOutOfFrameFence)
    {
        // wait for previous cmd with the same frame index
        Utils::WaitAndResetFence(device, frameFences[frameIndex]);
    }
    else
    {
        Utils::WaitAndResetFences(device, frameFences[frameIndex], outOfFrameFences[frameIndex]);
    }

    swapchain->RequestVsync(startInfo.requestVSync);
    swapchain->AcquireImage(imageAvailableSemaphores[frameIndex]);

    VkSemaphore semaphoreToWaitOnSubmit = imageAvailableSemaphores[frameIndex];


    // if out-of-frame cmd exist, submit it
    {
        VkCommandBuffer preFrameCmd = currentFrameState.GetPreFrameCmdAndRemove();
        if (preFrameCmd != VK_NULL_HANDLE)
        {
            // Signal inFrameSemaphore after completion.
            // Signal outOfFrameFences, but for the next frame
            // because we can't reset cmd pool with cmds (in this case 
            // it's preFrameCmd) that are in use.
            cmdManager->Submit(preFrameCmd,
                               semaphoreToWaitOnSubmit, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               inFrameSemaphores[frameIndex],
                               outOfFrameFences[(frameIndex + 1) % MAX_FRAMES_IN_FLIGHT]);

            // should wait other semaphore in this case
            semaphoreToWaitOnSubmit = inFrameSemaphores[frameIndex];

            waitForOutOfFrameFence = true;
        }
        else
        {
            waitForOutOfFrameFence = false;
        }
    }
    currentFrameState.SetSemaphore(semaphoreToWaitOnSubmit);


    if (startInfo.requestShaderReload)
    {
        shaderManager->ReloadShaders();
    }


    // reset cmds for current frame index
    cmdManager->PrepareForFrame(frameIndex);

    // clear the data that were created MAX_FRAMES_IN_FLIGHT ago
    worldSamplerManager->PrepareForFrame(frameIndex);
    genericSamplerManager->PrepareForFrame(frameIndex);
    textureManager->PrepareForFrame(frameIndex);
    cubemapManager->PrepareForFrame(frameIndex);
    rasterizer->PrepareForFrame(frameIndex);
    decalManager->PrepareForFrame(frameIndex);

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    BeginCmdLabel(cmd, "Prepare for frame");

    // start dynamic geometry recording to current frame
    scene->PrepareForFrame(cmd, frameIndex);

    return cmd;
}

#define RG_SET_VEC3( dst, x, y, z ) \
    ( dst )[ 0 ] = ( x );           \
    ( dst )[ 1 ] = ( y );           \
    ( dst )[ 2 ] = ( z )

#define RG_SET_VEC3_A( dst, xyz )  \
    ( dst )[ 0 ] = ( xyz )[ 0 ]; \
    ( dst )[ 1 ] = ( xyz )[ 1 ]; \
    ( dst )[ 2 ] = ( xyz )[ 2 ]

#define RG_MAX_VEC3( dst, m ) \
    ( dst )[ 0 ] = std::max( ( dst )[ 0 ], ( m ) );  \
    ( dst )[ 1 ] = std::max( ( dst )[ 1 ], ( m ) );  \
    ( dst )[ 2 ] = std::max( ( dst )[ 2 ], ( m ) )

#define RG_SET_VEC4( dst, x, y, z, w ) \
    ( dst )[ 0 ] = ( x );           \
    ( dst )[ 1 ] = ( y );           \
    ( dst )[ 2 ] = ( z );           \
    ( dst )[ 3 ] = ( w )

void RTGL1::VulkanDevice::FillUniform( RTGL1::ShGlobalUniform *gu, const RgDrawFrameInfo &drawInfo) const
{
    const float IdentityMat4x4[16] =
    {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };

    const float aspect = static_cast< float >( renderResolution.Width() ) / static_cast< float >( renderResolution.Height() );

    {
        memcpy( gu->viewPrev, gu->view, 16 * sizeof( float ) );
        memcpy( gu->projectionPrev, gu->projection, 16 * sizeof( float ) );

        memcpy( gu->view, drawInfo.view, 16 * sizeof( float ) );

        Matrix::MakeProjectionMatrix( gu->projection, 
                                      aspect, 
                                      drawInfo.fovYRadians, 
                                      drawInfo.cameraNear, 
                                      drawInfo.cameraFar );

        Matrix::Inverse( gu->invView, gu->view );
        Matrix::Inverse( gu->invProjection, gu->projection );

        memcpy( gu->cameraPositionPrev, gu->cameraPosition, 3 * sizeof( float ) );
        gu->cameraPosition[ 0 ] = gu->invView[ 12 ];
        gu->cameraPosition[ 1 ] = gu->invView[ 13 ];
        gu->cameraPosition[ 2 ] = gu->invView[ 14 ];
    }

    {
        static_assert( sizeof( gu->instanceGeomInfoOffset ) == sizeof( gu->instanceGeomInfoOffsetPrev ) );
        memcpy( gu->instanceGeomInfoOffsetPrev, gu->instanceGeomInfoOffset, sizeof( gu->instanceGeomInfoOffset ) );
    }

    {
        gu->frameId   = frameId;
        gu->timeDelta = static_cast< float >( std::max< double >( currentFrameTime - previousFrameTime, 0.001 ) );
        gu->time      = static_cast< float >( currentFrameTime );
    }

    {
        gu->renderWidth  = static_cast< float >( renderResolution.Width() );
        gu->renderHeight = static_cast< float >( renderResolution.Height() );
        // render width must be always even for checkerboarding!
        assert( ( int )gu->renderWidth % 2 == 0 );

        gu->upscaledRenderWidth  = static_cast< float >( renderResolution.UpscaledWidth() );
        gu->upscaledRenderHeight = static_cast< float >( renderResolution.UpscaledHeight() );

        RgFloat2D jitter = renderResolution.IsNvDlssEnabled() ? HaltonSequence::GetJitter_Halton23( frameId ) :
                           renderResolution.IsAmdFsr2Enabled() ? FSR2::GetJitter( renderResolution.GetResolutionState(), frameId ) :
                           RgFloat2D{ 0, 0 };

        gu->jitterX = jitter.data[ 0 ];
        gu->jitterY = jitter.data[ 1 ];
    }

    {
        gu->stopEyeAdaptation = drawInfo.disableEyeAdaptation;

        if( drawInfo.pTonemappingParams != nullptr )
        {
            gu->minLogLuminance     = drawInfo.pTonemappingParams->minLogLuminance;
            gu->maxLogLuminance     = drawInfo.pTonemappingParams->maxLogLuminance;
            gu->luminanceWhitePoint = drawInfo.pTonemappingParams->luminanceWhitePoint;
        }
        else
        {
            gu->minLogLuminance     = -3.9f;
            gu->maxLogLuminance     = -2.8f;
            gu->luminanceWhitePoint = 10.0f;
        }
    }

    {
        gu->lightCount     = scene->GetLightManager()->GetLightCount();
        gu->lightCountPrev = scene->GetLightManager()->GetLightCountPrev();

        gu->directionalLightExists = scene->GetLightManager()->DoesDirectionalLightExist();
    }

    {
        static_assert( sizeof( gu->skyCubemapRotationTransform ) == sizeof( IdentityMat4x4 ) && 
                       sizeof( IdentityMat4x4 ) == 16 * sizeof( float ), "Recheck skyCubemapRotationTransform sizes" );
        memcpy( gu->skyCubemapRotationTransform, IdentityMat4x4, 16 * sizeof( float ) );

        if( drawInfo.pSkyParams != nullptr )
        {
            const auto& sp = *drawInfo.pSkyParams;

            RG_SET_VEC3_A( gu->skyColorDefault, sp.skyColorDefault.data );
            gu->skyColorMultiplier = sp.skyColorMultiplier;
            gu->skyColorSaturation = std::max( sp.skyColorSaturation, 0.0f );

            gu->skyType = sp.skyType == RG_SKY_TYPE_CUBEMAP ? SKY_TYPE_CUBEMAP :
                          sp.skyType == RG_SKY_TYPE_RASTERIZED_GEOMETRY ? SKY_TYPE_RASTERIZED_GEOMETRY :
                          SKY_TYPE_COLOR;

            gu->skyCubemapIndex = cubemapManager->TryGetTextureIndex( sp.pSkyCubemapTextureName );

            if( !Utils::IsAlmostZero( drawInfo.pSkyParams->skyCubemapRotationTransform ) )
            {
                Utils::SetMatrix3ToGLSLMat4( gu->skyCubemapRotationTransform, drawInfo.pSkyParams->skyCubemapRotationTransform );
            }
        }
        else
        {
            RG_SET_VEC3( gu->skyColorDefault, 1.0f, 1.0f, 1.0f );
            gu->skyColorMultiplier = 1.0f;
            gu->skyColorSaturation = 1.0f;
            gu->skyType            = SKY_TYPE_COLOR;
            gu->skyCubemapIndex    = MATERIAL_NO_TEXTURE;
        }

        RgFloat3D skyViewerPosition = drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : RgFloat3D{ 0, 0, 0 };

        for( uint32_t i = 0; i < 6; i++ )
        {
            float* viewProjDst = &gu->viewProjCubemap[ 16 * i ];

            Matrix::GetCubemapViewProjMat( viewProjDst, i, skyViewerPosition.data, drawInfo.cameraNear, drawInfo.cameraFar );
        }
    }

    gu->debugShowFlags = 0;

    if( drawInfo.pDebugParams != nullptr )
    {
        RgDebugDrawFlags fs = drawInfo.pDebugParams->drawFlags;

        if( fs & RG_DEBUG_DRAW_ONLY_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_ONLY_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_ONLY_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR;
        }

        if( fs & RG_DEBUG_DRAW_ALBEDO_WHITE_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ALBEDO_WHITE;
        }
        if( fs & RG_DEBUG_DRAW_MOTION_VECTORS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_MOTION_VECTORS;
        }
        if( fs & RG_DEBUG_DRAW_GRADIENTS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_GRADIENTS;
        }
        if( fs & RG_DEBUG_DRAW_LIGHT_GRID_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_LIGHT_GRID;
        }
    }

    if( drawInfo.pTexturesParams != nullptr )
    {
        gu->normalMapStrength      = drawInfo.pTexturesParams->normalMapStrength;
        gu->emissionMapBoost       = std::max( drawInfo.pTexturesParams->emissionMapBoost, 0.0f );
        gu->emissionMaxScreenColor = std::max( drawInfo.pTexturesParams->emissionMaxScreenColor, 0.0f );
        gu->squareInputRoughness   = !!drawInfo.pTexturesParams->squareInputRoughness;
        gu->minRoughness           = std::clamp( drawInfo.pTexturesParams->minRoughness, 0.0f, 1.0f );
    }
    else
    {
        gu->normalMapStrength      = 1.0f;
        gu->emissionMapBoost       = 100.0f;
        gu->emissionMaxScreenColor = 1.5f;
        gu->squareInputRoughness   = 1;
        gu->minRoughness           = 0.0f;
    }

    if( drawInfo.pIlluminationParams != nullptr )
    {
        gu->maxBounceShadowsLights     = drawInfo.pIlluminationParams->maxBounceShadows;
        gu->polyLightSpotlightFactor   = std::max( 0.0f, drawInfo.pIlluminationParams->polygonalLightSpotlightFactor );
        gu->indirSecondBounce          = !!drawInfo.pIlluminationParams->enableSecondBounceForIndirect;
        gu->lightIndexIgnoreFPVShadows = scene->GetLightManager()->GetLightIndexIgnoreFPVShadows( currentFrameState.GetFrameIndex(), drawInfo.pIlluminationParams->lightUniqueIdIgnoreFirstPersonViewerShadows );
        gu->cellWorldSize              = std::max( drawInfo.pIlluminationParams->cellWorldSize, 0.001f );
        gu->gradientMultDiffuse        = std::clamp( drawInfo.pIlluminationParams->directDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultIndirect       = std::clamp( drawInfo.pIlluminationParams->indirectDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultSpecular       = std::clamp( drawInfo.pIlluminationParams->specularSensitivityToChange, 0.0f, 1.0f );
    }
    else
    {
        gu->maxBounceShadowsLights     = 2;
        gu->polyLightSpotlightFactor   = 2.0f;
        gu->indirSecondBounce          = true;
        gu->lightIndexIgnoreFPVShadows = LIGHT_INDEX_NONE;
        gu->cellWorldSize              = 1.0f;
        gu->gradientMultDiffuse        = 0.5f;
        gu->gradientMultIndirect       = 0.2f;
        gu->gradientMultSpecular       = 0.5f;
    }

    if( drawInfo.pBloomParams != nullptr )
    {
        gu->bloomThreshold              = std::max( drawInfo.pBloomParams->inputThreshold, 0.0f );
        gu->bloomIntensity              = std::max( drawInfo.pBloomParams->bloomIntensity, 0.0f );
        gu->bloomEmissionMultiplier     = std::max( drawInfo.pBloomParams->bloomEmissionMultiplier, 0.0f );
    }
    else
    {
        gu->bloomThreshold              = 15.0f;
        gu->bloomIntensity              = 1.0f;
        gu->bloomEmissionMultiplier     = 64.0f;
    }

    static_assert(
        RG_MEDIA_TYPE_VACUUM == MEDIA_TYPE_VACUUM &&
        RG_MEDIA_TYPE_WATER == MEDIA_TYPE_WATER &&
        RG_MEDIA_TYPE_GLASS == MEDIA_TYPE_GLASS && 
        RG_MEDIA_TYPE_ACID == MEDIA_TYPE_ACID,
        "Interface and GLSL constants must be identical" );

    if( drawInfo.pReflectRefractParams != nullptr )
    {
        const auto& rr = *drawInfo.pReflectRefractParams;

        if( rr.typeOfMediaAroundCamera >= 0 && rr.typeOfMediaAroundCamera < MEDIA_TYPE_COUNT )
        {
            gu->cameraMediaType = rr.typeOfMediaAroundCamera;
        }
        else
        {
            gu->cameraMediaType = MEDIA_TYPE_VACUUM;
        }

        gu->reflectRefractMaxDepth     = std::min( 4u, rr.maxReflectRefractDepth );

        gu->indexOfRefractionGlass = std::max( 0.0f, rr.indexOfRefractionGlass );
        gu->indexOfRefractionWater = std::max( 0.0f, rr.indexOfRefractionWater );

        memcpy( gu->waterColorAndDensity, rr.waterColor.data, 3 * sizeof( float ) );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        memcpy( gu->acidColorAndDensity, rr.acidColor.data, 3 * sizeof( float ) );
        gu->acidColorAndDensity[ 3 ] = rr.acidDensity;

        gu->forceNoWaterRefraction            = !!rr.forceNoWaterRefraction;
        gu->waterWaveSpeed                    = rr.waterWaveSpeed;
        gu->waterWaveStrength                 = rr.waterWaveNormalStrength;
        gu->waterTextureDerivativesMultiplier = std::max( 0.0f, rr.waterWaveTextureDerivativesMultiplier );
        if( rr.waterTextureAreaScale < 0.0001f )
        {
            gu->waterTextureAreaScale = 1.0f;
        }
        else
        {
            gu->waterTextureAreaScale = rr.waterTextureAreaScale;
        }

        gu->noBackfaceReflForNoMediaChange = !!rr.disableBackfaceReflectionsForNoMediaChange;

        gu->twirlPortalNormal = !!rr.portalNormalTwirl;
    }
    else
    {
        gu->cameraMediaType        = MEDIA_TYPE_VACUUM;
        gu->reflectRefractMaxDepth = 2;

        gu->indexOfRefractionGlass = 1.52f;
        gu->indexOfRefractionWater = 1.33f;

        RG_SET_VEC3( gu->waterColorAndDensity, 0.3f, 0.73f, 0.63f );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        RG_SET_VEC3( gu->acidColorAndDensity, 0.0f, 0.66f, 0.55f );
        gu->acidColorAndDensity[ 3 ] = 10.0f;

        gu->forceNoWaterRefraction            = false;
        gu->waterWaveSpeed                    = 1.0f;
        gu->waterWaveStrength                 = 1.0f;
        gu->waterTextureDerivativesMultiplier = 1.0f;
        gu->waterTextureAreaScale             = 1.0f;

        gu->noBackfaceReflForNoMediaChange = false;

        gu->twirlPortalNormal = false;
    }

    gu->rayCullBackFaces  = rayCullBackFacingTriangles ? 1 : 0;
    gu->rayLength         = clamp( drawInfo.rayLength, 0.1f, ( float )MAX_RAY_LENGTH );
    gu->primaryRayMinDist = clamp( drawInfo.cameraNear, 0.001f, gu->rayLength );

    {
        gu->rayCullMaskWorld = 0;

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_0_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_0;
        }

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_1_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_1;
        }

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_2_BIT )
        {
            if( allowGeometryWithSkyFlag )
            {
                throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "RG_DRAW_FRAME_RAY_CULL_WORLD_2_BIT cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was true" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }

#if RAYCULLMASK_SKY_IS_WORLD2
        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_SKY_BIT )
        {
            if( !allowGeometryWithSkyFlag )
            {
                throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "RG_DRAW_FRAME_RAY_CULL_SKY_BIT cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was false" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }
#else
    #error Handle RG_DRAW_FRAME_RAY_CULL_SKY_BIT, if there is no WORLD_2
#endif


        if( allowGeometryWithSkyFlag )
        {
            gu->rayCullMaskWorld_Shadow = gu->rayCullMaskWorld & ( ~INSTANCE_MASK_WORLD_2 );
        }
        else
        {
            gu->rayCullMaskWorld_Shadow = gu->rayCullMaskWorld;
        }
    }

    gu->waterNormalTextureIndex = textureManager->GetWaterNormalTextureIndex();

    gu->cameraRayConeSpreadAngle = atanf( ( 2.0f * tanf( drawInfo.fovYRadians * 0.5f ) ) / ( float )renderResolution.Height() );

    if( Utils::IsAlmostZero( drawInfo.worldUpVector ) )
    {
        gu->worldUpVector[ 0 ] = 0.0f;
        gu->worldUpVector[ 1 ] = 1.0f;
        gu->worldUpVector[ 2 ] = 0.0f;
    }
    else
    {
        gu->worldUpVector[ 0 ] = drawInfo.worldUpVector.data[ 0 ];
        gu->worldUpVector[ 1 ] = drawInfo.worldUpVector.data[ 1 ];
        gu->worldUpVector[ 2 ] = drawInfo.worldUpVector.data[ 2 ];
    }

    if( drawInfo.pLightmapParams != nullptr )
    {
        gu->lightmapEnable = !!drawInfo.pLightmapParams->enableLightmaps;

        if( drawInfo.pLightmapParams->lightmapLayerIndex == 1 || drawInfo.pLightmapParams->lightmapLayerIndex == 2 )
        {
            gu->lightmapLayer = drawInfo.pLightmapParams->lightmapLayerIndex;
        }
        else
        {
            assert( 0 && "pLightMapLayerIndex must point to a value of 1 or 2. Others are invalidated" );
        }
    }
    else
    {
        gu->lightmapEnable = false;
        gu->lightmapLayer  = UINT8_MAX;
    }

    gu->lensFlareCullingInputCount = rasterizer->GetLensFlareCullingInputCount();
    gu->applyViewProjToLensFlares  = false;

    {
        gu->volumeCameraNear = std::max( drawInfo.cameraNear, 0.001f );
        gu->volumeCameraFar  = std::min(
            drawInfo.cameraFar,
            drawInfo.pVolumetricParams ? drawInfo.pVolumetricParams->volumetricFar : 100.0f );

        if( drawInfo.pVolumetricParams )
        {
            if( drawInfo.pVolumetricParams->enable )
            {
                gu->volumeEnableType = drawInfo.pVolumetricParams->useSimpleDepthBased
                                           ? VOLUME_ENABLE_SIMPLE
                                           : VOLUME_ENABLE_VOLUMETRIC;
            }
            else
            {
                gu->volumeEnableType = VOLUME_ENABLE_NONE;
            }
            gu->volumeScattering = drawInfo.pVolumetricParams->scaterring;
            gu->volumeSourceAsymmetry = std::clamp( drawInfo.pVolumetricParams->sourceAssymetry, -1.0f, 1.0f );

            RG_SET_VEC3_A( gu->volumeAmbient, drawInfo.pVolumetricParams->ambientColor.data );
            RG_MAX_VEC3( gu->volumeAmbient, 0.0f );

            RG_SET_VEC3_A( gu->volumeSourceColor, drawInfo.pVolumetricParams->sourceColor.data );
            RG_MAX_VEC3( gu->volumeSourceColor, 0.0f );

            RG_SET_VEC3_A( gu->volumeDirToSource, drawInfo.pVolumetricParams->sourceDirection.data );
            Utils::Negate( gu->volumeDirToSource );
            Utils::Normalize( gu->volumeDirToSource );
        }
        else
        {
            gu->volumeEnableType      = VOLUME_ENABLE_VOLUMETRIC;
            gu->volumeScattering      = 0.2f;
            gu->volumeSourceAsymmetry = 0.4f;
            RG_SET_VEC3( gu->volumeAmbient, 0.8f, 0.85f, 1.0f );
            RG_SET_VEC3( gu->volumeSourceColor, 0, 0, 0 );
            RG_SET_VEC3( gu->volumeDirToSource, 0, 1, 0 );
        }

        if( gu->volumeEnableType != VOLUME_ENABLE_NONE )
        {
            memcpy( gu->volumeViewProj_Prev, gu->volumeViewProj, 16 * sizeof( float ) );
            memcpy( gu->volumeViewProjInv_Prev, gu->volumeViewProjInv, 16 * sizeof( float ) );

            float volumeproj[ 16 ];
            Matrix::MakeProjectionMatrix( volumeproj,
                                          aspect,
                                          drawInfo.fovYRadians,
                                          gu->volumeCameraNear,
                                          gu->volumeCameraFar );

            Matrix::Multiply( gu->volumeViewProj, gu->view, volumeproj );
            Matrix::Inverse( gu->volumeViewProjInv, gu->volumeViewProj );
        }
    }

    gu->antiFireflyEnabled = !!drawInfo.forceAntiFirefly;
}

void RTGL1::VulkanDevice::Render(VkCommandBuffer cmd, const RgDrawFrameInfo &drawInfo)
{
    // end of "Prepare for frame" label
    EndCmdLabel(cmd);


    const uint32_t frameIndex = currentFrameState.GetFrameIndex();

    
    bool mipLodBiasUpdated = worldSamplerManager->TryChangeMipLodBias(frameIndex, renderResolution.GetMipLodBias());
    const RgFloat2D jitter = { uniform->GetData()->jitterX, uniform->GetData()->jitterY };

    textureManager->SubmitDescriptors(frameIndex, drawInfo.pTexturesParams, mipLodBiasUpdated);
    cubemapManager->SubmitDescriptors(frameIndex);


    // submit geometry and upload uniform after getting data from a scene
    scene->SubmitForFrame(cmd, frameIndex, uniform, 
                          uniform->GetData()->rayCullMaskWorld, 
                          allowGeometryWithSkyFlag,
                          drawInfo.disableRayTracedGeometry);


    framebuffers->PrepareForSize(renderResolution.GetResolutionState());
    

    if (!drawInfo.disableRasterization)
    {
        rasterizer->SubmitForFrame(cmd, frameIndex);

        // draw rasterized sky to albedo before tracing primary rays
        if (uniform->GetData()->skyType == RG_SKY_TYPE_RASTERIZED_GEOMETRY)
        {
            RgFloat3D skyViewerPosition = drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : RgFloat3D{ 0,0,0 };

            rasterizer->DrawSkyToCubemap(cmd, frameIndex, textureManager, uniform);
            rasterizer->DrawSkyToAlbedo(cmd, frameIndex, textureManager, uniform->GetData()->view, skyViewerPosition.data, uniform->GetData()->projection, jitter, renderResolution);
        }
    }


    {
        lightGrid->Build(cmd, frameIndex, uniform, blueNoise, scene->GetLightManager());

        decalManager->SubmitForFrame(cmd, frameIndex);
        portalList->SubmitForFrame(cmd, frameIndex);

        const auto params = pathTracer->Bind( cmd,
                                              frameIndex,
                                              renderResolution.Width(),
                                              renderResolution.Height(),
                                              scene.get(),
                                              uniform.get(),
                                              textureManager.get(),
                                              framebuffers,
                                              restirBuffers,
                                              blueNoise.get(),
                                              cubemapManager.get(),
                                              rasterizer->GetRenderCubemap().get(),
                                              portalList.get(),
                                              volumetric.get() );

        pathTracer->TracePrimaryRays(params);

        // draw decals on top of primary surface
        decalManager->Draw(cmd, frameIndex, uniform, framebuffers, textureManager);

        if (uniform->GetData()->reflectRefractMaxDepth > 0)
        {
            pathTracer->TraceReflectionRefractionRays(params);
        }

        scene->GetLightManager()->BarrierLightGrid(cmd, frameIndex);
        pathTracer->CalculateInitialReservoirs(params);
        pathTracer->TraceDirectllumination(params);
        pathTracer->TraceIndirectllumination(params);
        pathTracer->TraceVolumetric(params);

        pathTracer->CalculateGradientsSamples(params);
        denoiser->Denoise(cmd, frameIndex, uniform);
        volumetric->ProcessScattering( cmd, frameIndex, uniform.get(), blueNoise.get() );
        tonemapping->CalculateExposure(cmd, frameIndex, uniform);
    }

    imageComposition->PrepareForRaster( cmd, frameIndex, uniform.get() );
    volumetric->BarrierToReadScattering( cmd, frameIndex );
    volumetric->BarrierToReadIllumination( cmd );

    if (!drawInfo.disableRasterization)
    {
        // draw rasterized geometry into the final image
        rasterizer->DrawToFinalImage(
            cmd,
            frameIndex,
            textureManager,
            uniform,
            tonemapping,
            volumetric,
            uniform->GetData()->view,
            uniform->GetData()->projection,
            jitter,
            renderResolution,
            nullptr );
    }

    imageComposition->Finalize(
        cmd, frameIndex, uniform.get(), tonemapping.get(), volumetric.get() );


    bool enableBloom = drawInfo.pBloomParams == nullptr || (drawInfo.pBloomParams != nullptr && drawInfo.pBloomParams->bloomIntensity > 0.0f);

    if (enableBloom)
    {
        bloom->Prepare(cmd, frameIndex, uniform, tonemapping);
    }


    FramebufferImageIndex accum = FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
    {
        // upscale finalized image
        if (renderResolution.IsNvDlssEnabled())
        {
            accum = nvDlss->Apply(cmd, frameIndex, 
                                               framebuffers, 
                                               renderResolution, 
                                               jitter);
        }
        else if (renderResolution.IsAmdFsr2Enabled())
        {
            accum = amdFsr2->Apply(cmd, frameIndex, 
                                                framebuffers, 
                                                renderResolution, 
                                                jitter, 
                                                uniform->GetData()->timeDelta, 
                                                drawInfo.cameraNear, 
                                                drawInfo.cameraFar, 
                                                drawInfo.fovYRadians );
        }

        const RgExtent2D* pixelized = drawInfo.pRenderResolutionParams
                                          ? drawInfo.pRenderResolutionParams->pPixelizedRenderSize
                                          : nullptr;

        accum = framebuffers->BlitForEffects(
            cmd, frameIndex, accum, renderResolution.GetBlitFilter(), pixelized );
    }


    const CommonnlyUsedEffectArguments args = { cmd, frameIndex, framebuffers, uniform, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), (float)currentFrameTime };
    {
        if (renderResolution.IsDedicatedSharpeningEnabled())
        {
            accum = sharpening->Apply(
                cmd, frameIndex, framebuffers, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), accum,
                renderResolution.GetSharpeningTechnique(), renderResolution.GetSharpeningIntensity());
        }
        if (enableBloom)
        {
            accum = bloom->Apply(cmd, frameIndex, uniform, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), accum);
        }
        if (effectColorTint->Setup(args, drawInfo.postEffectParams.pColorTint))
        {
            accum = effectColorTint->Apply(args, accum);
        }
        if (effectInverseBW->Setup(args, drawInfo.postEffectParams.pInverseBlackAndWhite))
        {
            accum = effectInverseBW->Apply(args, accum);
        }
        if (effectHueShift->Setup(args, drawInfo.postEffectParams.pHueShift))
        {
            accum = effectHueShift->Apply(args, accum);
        }
        if (effectChromaticAberration->Setup(args, drawInfo.postEffectParams.pChromaticAberration))
        {
            accum = effectChromaticAberration->Apply(args, accum);
        }
        if (effectDistortedSides->Setup(args, drawInfo.postEffectParams.pDistortedSides))
        {
            accum = effectDistortedSides->Apply(args, accum);
        }
        if (effectWaves->Setup(args, drawInfo.postEffectParams.pWaves))
        {
            accum = effectWaves->Apply(args, accum);
        }
        if (effectRadialBlur->Setup(args, drawInfo.postEffectParams.pRadialBlur))
        {
            accum = effectRadialBlur->Apply(args, accum);
        }
    }

    // draw geometry such as HUD into an upscaled framebuf
    if (!drawInfo.disableRasterization)
    {
        rasterizer->DrawToSwapchain( cmd,
                                     frameIndex,
                                     accum,
                                     textureManager,
                                     uniform->GetData()->view,
                                     uniform->GetData()->projection,
                                     renderResolution.UpscaledWidth(),
                                     renderResolution.UpscaledHeight() );
    }

    // post-effect that work on swapchain geometry too
    {
        if (effectWipe->Setup(args, drawInfo.postEffectParams.pWipe, swapchain, frameId))
        {
            accum = effectWipe->Apply(args, blueNoise, accum);
        }
        if (drawInfo.postEffectParams.pCRT != nullptr && drawInfo.postEffectParams.pCRT->isActive)
        {
            effectCrtDemodulateEncode->Setup(args);
            accum = effectCrtDemodulateEncode->Apply(args, accum);

            effectCrtDecode->Setup(args);
            accum = effectCrtDecode->Apply(args, accum);
        }
    }

    // blit result image to present on a surface
    framebuffers->PresentToSwapchain( cmd, frameIndex, swapchain, accum, VK_FILTER_NEAREST );
}

void RTGL1::VulkanDevice::EndFrame(VkCommandBuffer cmd)
{
    uint32_t frameIndex = currentFrameState.GetFrameIndex();
    VkSemaphore semaphoreToWait = currentFrameState.GetSemaphoreForWaitAndRemove();

    // submit command buffer, but wait until presentation engine has completed using image
    cmdManager->Submit(
        cmd, 
        semaphoreToWait,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
        renderFinishedSemaphores[frameIndex],
        frameFences[frameIndex]);

    // present on a surface when rendering will be finished
    swapchain->Present(queues, renderFinishedSemaphores[frameIndex]);

    frameId++;
}



// Interface implementation



void RTGL1::VulkanDevice::StartFrame( const RgStartFrameInfo* pInfo )
{
    if( currentFrameState.WasFrameStarted() )
    {
        throw RgException( RG_RESULT_FRAME_WASNT_ENDED );
    }

    if( pInfo == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    VkCommandBuffer newFrameCmd = BeginFrame( *pInfo );
    currentFrameState.OnBeginFrame( newFrameCmd );
}

void RTGL1::VulkanDevice::DrawFrame( const RgDrawFrameInfo* pInfo )
{
    if( !currentFrameState.WasFrameStarted() )
    {
        throw RgException( RG_RESULT_FRAME_WASNT_STARTED );
    }

    if( pInfo == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    VkCommandBuffer cmd = currentFrameState.GetCmdBuffer();

    previousFrameTime = currentFrameTime;
    currentFrameTime  = pInfo->currentTime;

    renderResolution.Setup( pInfo->pRenderResolutionParams,
                            swapchain->GetWidth(),
                            swapchain->GetHeight(),
                            nvDlss );

    textureManager->CheckForHotReload( cmd );

    if( renderResolution.Width() > 0 && renderResolution.Height() > 0 )
    {
        FillUniform( uniform->GetData(), *pInfo );
        Render( cmd, *pInfo );
    }

    EndFrame( cmd );
    currentFrameState.OnEndFrame();
}

void RTGL1::VulkanDevice::UploadMeshPrimitive( const RgMeshInfo*          pMesh,
                                               const RgMeshPrimitiveInfo* pPrimitive,
                                               uint32_t                   primitiveIndexInMesh )
{
    if( pMesh == nullptr || pPrimitive == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }
}

void RTGL1::VulkanDevice::UploadNonWorldPrimitive( const RgMeshPrimitiveInfo* pPrimitive, const float* pViewProjection, const RgViewport* pViewport )
{
    if( pPrimitive == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }
}

void RTGL1::VulkanDevice::UploadDecal(const RgDecalUploadInfo *pInfo)
{
    if( pInfo == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    decalManager->Upload( currentFrameState.GetFrameIndex(), *pInfo, textureManager );
}

void RTGL1::VulkanDevice::UploadDirectionalLight(const RgDirectionalLightUploadInfo *pInfo)
{
    if (pInfo == nullptr)
    {
        throw RgException(RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null");
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pInfo);
}

void RTGL1::VulkanDevice::UploadSphericalLight(const RgSphericalLightUploadInfo *pInfo)
{
    if (pInfo == nullptr)
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pInfo);
}

void RTGL1::VulkanDevice::UploadSpotlight(const RgSpotLightUploadInfo *pInfo)
{
    if (pInfo == nullptr)
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pInfo);
}

void RTGL1::VulkanDevice::UploadPolygonalLight(const RgPolygonalLightUploadInfo *pInfo)
{
    if (pInfo == nullptr)
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pInfo);
}

void RTGL1::VulkanDevice::ProvideOriginalTexture( const RgOriginalTextureInfo* pInfo )
{
    if( pInfo == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    textureManager->CreateMaterial( currentFrameState.GetCmdBufferForMaterials( cmdManager ), currentFrameState.GetFrameIndex(), *pInfo );
}

void RTGL1::VulkanDevice::ProvideOriginalCubemapTexture( const RgCubemapCreateInfo* pInfo )
{
    if( pInfo == nullptr )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Argument is null" );
    }

    cubemapManager->CreateCubemap( currentFrameState.GetCmdBufferForMaterials( cmdManager ), currentFrameState.GetFrameIndex(), *pInfo );
}

void RTGL1::VulkanDevice::MarkOriginalTextureAsDeleted( const char* pTextureName )
{
    textureManager->TryDestroyMaterial( currentFrameState.GetFrameIndex(), pTextureName );
    cubemapManager->TryDestroyMaterial( currentFrameState.GetFrameIndex(), pTextureName );
}

bool RTGL1::VulkanDevice::IsSuspended() const
{
    if( !swapchain )
    {
        return false;
    }

    return !swapchain->IsExtentOptimal();
}

bool RTGL1::VulkanDevice::IsUpscaleTechniqueAvailable( RgRenderUpscaleTechnique technique ) const
{
    switch( technique )
    {
        case RG_RENDER_UPSCALE_TECHNIQUE_NEAREST:
        case RG_RENDER_UPSCALE_TECHNIQUE_LINEAR:
        case RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2: return true;

        case RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS: return nvDlss->IsDlssAvailable();

        default: throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT, "Incorrect technique was passed to rgIsRenderUpscaleTechniqueAvailable" );
    }
}

void RTGL1::VulkanDevice::ExportAsPNG( const void* pPixels, uint32_t width, uint32_t height, const char* pPath ) const
{
    
}

RgPrimitiveVertex* RTGL1::VulkanDevice::ScratchAllocForVertices( uint32_t count ) {}

void               RTGL1::VulkanDevice::ScratchFree( const RgPrimitiveVertex* pPointer ) {}

void               RTGL1::VulkanDevice::Print( const char* pMessage, RgMessageSeverityFlags severity ) const
{
    userPrint->Print( pMessage, severity );
}