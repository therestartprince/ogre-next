/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2023 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "Compositor/Pass/PassWarmUp/OgreCompositorPassWarmUp.h"

#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorNode.h"
#include "Compositor/OgreCompositorShadowNode.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "Compositor/OgreCompositorWorkspaceListener.h"
#include "Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h"
#include "Compositor/Pass/PassWarmUp/OgreCompositorPassWarmUpDef.h"
#include "OgreCamera.h"
#include "OgreSceneManager.h"

namespace Ogre
{
    //-----------------------------------------------------------------------------------
    CompositorPassWarmUp::CompositorPassWarmUp( const CompositorPassWarmUpDef *definition,
                                                Camera *defaultCamera, CompositorNode *parentNode,
                                                const RenderTargetViewDef *rtv ) :
        CompositorPass( definition, parentNode ),
        mDefinition( definition ),
        mShadowNode( 0 ),
        mCamera( 0 )
    {
        initialize( rtv );

        CompositorWorkspace *workspace = parentNode->getWorkspace();

        if( mDefinition->mShadowNode != IdString() )
        {
            bool shadowNodeCreated;
            mShadowNode =
                workspace->findOrCreateShadowNode( mDefinition->mShadowNode, shadowNodeCreated );
        }

        if( mDefinition->mCameraName != IdString() )
            mCamera = workspace->findCamera( mDefinition->mCameraName );
        else
            mCamera = defaultCamera;
    }
    //-----------------------------------------------------------------------------------
    void CompositorPassWarmUp::notifyPassSceneAfterShadowMapsListeners()
    {
        const CompositorWorkspaceListenerVec &listeners = mParentNode->getWorkspace()->getListeners();
        for( CompositorWorkspaceListener *listener : listeners )
            listener->passSceneAfterShadowMaps( nullptr );
    }
    //-----------------------------------------------------------------------------------
    void CompositorPassWarmUp::execute( const Camera *lodCamera )
    {
        // Execute a limited number of times?
        if( mNumPassesLeft != std::numeric_limits<uint32>::max() )
        {
            if( !mNumPassesLeft )
                return;
            --mNumPassesLeft;
        }

        profilingBegin();

        notifyPassEarlyPreExecuteListeners();

        SceneManager *sceneManager = mCamera->getSceneManager();

        Viewport *viewport = sceneManager->getCurrentViewport0();
        viewport->_setVisibilityMask( mDefinition->mVisibilityMask, 0xFFFFFFFFu );

        CompositorShadowNode *shadowNode =
            ( mShadowNode && mShadowNode->getEnabled() ) ? mShadowNode : 0;
        sceneManager->_setCurrentShadowNode( shadowNode );

        // Fire the listener in case it wants to change anything
        notifyPassPreExecuteListeners();

        if( shadowNode )
        {
            // We need to prepare for rendering another RT (we broke the contiguous chain)
            if( mDefinition->mSkipLoadStoreSemantics )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "mSkipLoadStoreSemantics can't be true if updating the shadow node. You "
                             "can use shadow_node reuse",
                             "CompositorPassScene::execute" );
            }

            // Save the value in case the listener changed it
            const uint32 oldVisibilityMask = viewport->getVisibilityMask();
            const uint32 oldLightVisibilityMask = viewport->getLightVisibilityMask();

            // use culling camera for shadows, so if shadows are re used for slightly different camera
            // (ie VR) shadows are not 'over culled'
            mCamera->_notifyViewport( viewport );

            shadowNode->_update( mCamera, lodCamera, sceneManager );

            // ShadowNode passes may've overriden these settings.
            sceneManager->_setCurrentShadowNode( shadowNode );
            viewport->_setVisibilityMask( oldVisibilityMask, oldLightVisibilityMask );
            mCamera->_notifyViewport( viewport );
            // We need to restore the previous RT's update
        }

        notifyPassSceneAfterShadowMapsListeners();

        analyzeBarriers();
        executeResourceTransitions();

        setRenderPassDescToCurrent();

        sceneManager->_setCamerasInProgress( CamerasInProgress( mCamera ) );
        sceneManager->_setForwardPlusEnabledInPass( mDefinition->mEnableForwardPlus );
        // TODO
        // sceneManager->_setRefractions( mDepthTextureNoMsaa, mRefractionsTexture );
        sceneManager->_setCurrentCompositorPass( this );

        RenderSystem *renderSystem = sceneManager->getDestinationRenderSystem();
        renderSystem->executeRenderPassDescriptorDelayedActions();

        if( mDefinition->mMode & CompositorPassWarmUpDef::Collect )
        {
            sceneManager->_warmUpShadersCollect( mCamera, mDefinition->mVisibilityMask,
                                                 mDefinition->mFirstRQ, mDefinition->mLastRQ );
        }
        if( mDefinition->mMode & CompositorPassWarmUpDef::Trigger )
            sceneManager->_warmUpShadersTrigger();

        sceneManager->_setCurrentCompositorPass( 0 );

        notifyPassPosExecuteListeners();

        profilingEnd();
    }

    //-----------------------------------------------------------------------------------
    size_t WarmUpHelper::calculateNumTargetPasses( const CompositorNodeDef *refNode )
    {
        size_t numTargetPassesNeeded = 0u;

        const size_t numTargetPasses = refNode->getNumTargetPasses();
        for( size_t i = 0u; i < numTargetPasses; ++i )
        {
            const CompositorTargetDef *baseTargetDef = refNode->getTargetPass( i );
            const CompositorPassDefVec &basePassDefs = baseTargetDef->getCompositorPasses();

            for( const CompositorPassDef *basePassDef : basePassDefs )
            {
                if( basePassDef->getType() == PASS_SCENE )
                {
                    ++numTargetPassesNeeded;
                    break;
                }
            }
        }

        if( numTargetPassesNeeded == 0u )
        {
            OGRE_EXCEPT(
                Exception::ERR_INVALIDPARAMS,
                "Reference Node: " + refNode->getNameStr() + " has no scene passes to base from.",
                "WarmUpHelper::calculateNumTargetPasses" );
        }

        return numTargetPassesNeeded;
    }
    //-----------------------------------------------------------------------------------
    size_t WarmUpHelper::calculateNumScenePasses( const CompositorTargetDef *baseTargetDef )
    {
        size_t numScenePasses = 0u;
        const CompositorPassDefVec &basePassDefs = baseTargetDef->getCompositorPasses();

        for( const CompositorPassDef *basePassDef : basePassDefs )
        {
            if( basePassDef->getType() == PASS_SCENE )
                ++numScenePasses;
        }
        return numScenePasses;
    }
    //-----------------------------------------------------------------------------------
    void WarmUpHelper::createFromRtv( CompositorNodeDef *warmUpNodeDef, const CompositorNodeDef *refNode,
                                      const IdString textureName, std::set<IdString> &seenTextures )
    {
        if( seenTextures.find( textureName ) == seenTextures.end() )
        {
            size_t texIdx;
            TextureDefinitionBase::TextureSource texSource;
            refNode->getTextureSource( textureName, texIdx, texSource );

            if( texSource == TextureDefinitionBase::TEXTURE_LOCAL )
            {
                // Don't waste huge amounts of VRAM
                TextureDefinitionBase::TextureDefinition *texDef =
                    warmUpNodeDef->_addTextureDefinition( textureName );
                *texDef = refNode->getLocalTextureDefinitions()[texIdx];
                texDef->width = 4u;
                texDef->height = 4u;
                texDef->widthFactor = 1.0f;
                texDef->heightFactor = 1.0f;
            }
            else
            {
                warmUpNodeDef->_addTextureSourceName( textureName, texIdx, texSource );
            }
            seenTextures.insert( textureName );
        }
    }
    //-----------------------------------------------------------------------------------
    void WarmUpHelper::createFromRtv( CompositorNodeDef *warmUpNodeDef, const CompositorNodeDef *refNode,
                                      const RenderTargetViewEntry &rtvEntry,
                                      std::set<IdString> &seenTextures )
    {
        if( rtvEntry.textureName != IdString() )
        {
            createFromRtv( warmUpNodeDef, refNode, rtvEntry.textureName, seenTextures );
        }
        if( rtvEntry.textureName != rtvEntry.resolveTextureName &&
            rtvEntry.resolveTextureName != IdString() )
        {
            createFromRtv( warmUpNodeDef, refNode, rtvEntry.resolveTextureName, seenTextures );
        }
    }
    //-----------------------------------------------------------------------------------
    void WarmUpHelper::createFrom( CompositorManager2 *compositorManager,
                                   const String &nodeDefinitionName,
                                   const IdString refNodeDefinitionName )
    {
        std::set<IdString> seenTextures;
        std::set<IdString> seenRtv;

        const CompositorNodeDef *refNode = compositorManager->getNodeDefinition( refNodeDefinitionName );
        const size_t numTargetPassesNeeded = calculateNumTargetPasses( refNode );

        // Create the node definition
        CompositorNodeDef *warmUpNodeDef = compositorManager->addNodeDefinition( nodeDefinitionName );
        warmUpNodeDef->setNumTargetPass( numTargetPassesNeeded );

        const size_t numTargetPasses = refNode->getNumTargetPasses();
        for( size_t i = 0u; i < numTargetPasses; ++i )
        {
            const CompositorTargetDef *refTargetDef = refNode->getTargetPass( i );

            const size_t numScenePasses = calculateNumScenePasses( refTargetDef );

            if( numScenePasses > 0u )
            {
                // Copy RTV and textures
                if( seenRtv.find( refTargetDef->getRenderTargetName() ) == seenRtv.end() )
                {
                    const RenderTargetViewDef *refRtv =
                        refNode->getRenderTargetViewDef( refTargetDef->getRenderTargetName() );

                    RenderTargetViewDef *warmUpRtv =
                        warmUpNodeDef->addRenderTextureView( refTargetDef->getRenderTargetName() );
                    *warmUpRtv = *refRtv;

                    for( const RenderTargetViewEntry &rtvEntry : refRtv->colourAttachments )
                        createFromRtv( warmUpNodeDef, refNode, rtvEntry, seenTextures );
                    createFromRtv( warmUpNodeDef, refNode, refRtv->depthAttachment, seenTextures );
                    createFromRtv( warmUpNodeDef, refNode, refRtv->stencilAttachment, seenTextures );

                    seenRtv.insert( refTargetDef->getRenderTargetName() );
                }

                CompositorTargetDef *targetDef = warmUpNodeDef->addTargetPass(
                    refTargetDef->getRenderTargetNameStr(), refTargetDef->getRtIndex() );
                targetDef->setNumPasses( numScenePasses );

                targetDef->setTargetLevelBarrier( refTargetDef->getTargetLevelBarrier() );

                size_t currPassNum = 0u;
                const CompositorPassDefVec &refPassDefs = refTargetDef->getCompositorPasses();
                for( const CompositorPassDef *refPassDef : refPassDefs )
                {
                    if( refPassDef->getType() == PASS_SCENE )
                    {
                        CompositorPassDef *pass = targetDef->addPass( PASS_WARM_UP );

                        OGRE_ASSERT_HIGH( dynamic_cast<const CompositorPassSceneDef *>( refPassDef ) );
                        OGRE_ASSERT_HIGH( dynamic_cast<CompositorPassWarmUpDef *>( pass ) );

                        const CompositorPassSceneDef *refPassScene =
                            static_cast<const CompositorPassSceneDef *>( refPassDef );
                        CompositorPassWarmUpDef *passWarmUp =
                            static_cast<CompositorPassWarmUpDef *>( pass );

                        passWarmUp->mIdentifier = refPassScene->mIdentifier;
                        passWarmUp->mSkipLoadStoreSemantics = refPassScene->mSkipLoadStoreSemantics;
                        passWarmUp->mColourWrite = refPassScene->mColourWrite;
                        passWarmUp->mReadOnlyDepth = refPassScene->mReadOnlyDepth;
                        passWarmUp->mReadOnlyStencil = refPassScene->mReadOnlyStencil;
                        passWarmUp->mIncludeOverlays = refPassScene->mIncludeOverlays;
                        passWarmUp->mExecutionMask = refPassScene->mExecutionMask;
                        passWarmUp->mShadowMapFullViewport = refPassScene->mShadowMapFullViewport;
                        passWarmUp->mExposedTextures = refPassScene->mExposedTextures;
                        passWarmUp->mProfilingId = refPassScene->mProfilingId;

                        passWarmUp->mVisibilityMask = refPassScene->mVisibilityMask;
                        passWarmUp->mShadowNode = refPassScene->mShadowNode;
                        passWarmUp->mFirstRQ = refPassScene->mFirstRQ;
                        passWarmUp->mLastRQ = refPassScene->mLastRQ;
                        passWarmUp->mEnableForwardPlus = refPassScene->mEnableForwardPlus;

                        passWarmUp->mMode = CompositorPassWarmUpDef::Collect;

                        ++currPassNum;

                        if( i + 1u == numTargetPasses && currPassNum == numScenePasses )
                        {
                            // This is the last pass. Time to trigger everything we collected.
                            passWarmUp->mMode = CompositorPassWarmUpDef::CollectAndTrigger;
                        }
                    }
                }
            }
        }
    }
}  // namespace Ogre
