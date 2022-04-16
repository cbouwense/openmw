#include "postprocessor.hpp"

#include <SDL_opengl_glext.h>

#include <osg/Texture1D>
#include <osg/Texture2D>
#include <osg/Texture3D>

#include <components/settings/settings.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/util.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/misc/stringops.hpp>
#include <components/vfs/manager.hpp>

#include "../mwbase/world.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwgui/postprocessorhud.hpp"

#include "transparentpass.hpp"
#include "pingpongcull.hpp"
#include "renderingmanager.hpp"
#include "vismask.hpp"
#include "sky.hpp"

namespace
{
    struct ResizedCallback : osg::GraphicsContext::ResizedCallback
    {
        ResizedCallback(MWRender::PostProcessor* postProcessor)
            : mPostProcessor(postProcessor)
        { }

        void resizedImplementation(osg::GraphicsContext* gc, int x, int y, int width, int height) override
        {
            gc->resizedImplementation(x, y, width, height);

            mPostProcessor->resize(width, height, true);
        }

        MWRender::PostProcessor* mPostProcessor;
    };
}

namespace MWRender
{
    void ResolveFboInterceptor::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        traverse(node, cv);

        osgUtil::RenderStage* rs = cv->getRenderStage();

        auto& fbo = mFbos[cv->getTraversalNumber() % 2];

        if (rs && rs->getMultisampleResolveFramebufferObject() && fbo)
            rs->setMultisampleResolveFramebufferObject(fbo);
    }

    void ResolveFboInterceptor::setFbo(size_t frameId, osg::ref_ptr<osg::FrameBufferObject> fbo)
    {
        mFbos[frameId] = new osg::FrameBufferObject;
        mFbos[frameId]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, fbo->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0));
    }

    PostProcessor::PostProcessor(RenderingManager& rendering, osgViewer::Viewer* viewer, osg::Group* rootNode, const VFS::Manager* vfs)
        : mDepthFormat(GL_DEPTH24_STENCIL8)
        , mSamples(Settings::Manager::getInt("antialiasing", "Video"))
        , mDirty{true, true}
        , mRendering(rendering)
        , mViewer(viewer)
        , mVFS(vfs)
        , mReload(false)
        , mEnabled(false)
        , mUsePostProcessing(false)
        , mSoftParticles(false)
        , mDisableDepthPasses(false)
        , mExteriorFlag(false)
        , mUnderwater(false)
        , mHDR(false)
        , mMainTemplate(new osg::Texture2D)
    {
        bool softParticles = Settings::Manager::getBool("soft particles", "Shaders");
        bool usePostProcessing = Settings::Manager::getBool("enabled", "Post Processing");

        osg::GraphicsContext* gc = viewer->getCamera()->getGraphicsContext();
        unsigned int contextID = gc->getState()->getContextID();
        osg::GLExtensions* ext = gc->getState()->get<osg::GLExtensions>();

        mUBO = ext && ext->isUniformBufferObjectSupported && ext->glslLanguageVersion >= 3.3f;
        mStateUpdater = new fx::StateUpdater(mUBO);

        if (!SceneUtil::AutoDepth::isReversed() && !softParticles && !usePostProcessing)
            return;

        if (!usePostProcessing && !SceneUtil::AutoDepth::isReversed() && !softParticles)
        {
            Log(Debug::Info) << "Rendering to default framebuffer.";
            return;
        }

        if (!ext->isFrameBufferObjectSupported)
        {
            Log(Debug::Warning) << "Post processing and reverse-z disabled, FBO unsupported.";
            return;
        }

        if (mSamples > 1 && !ext->isRenderbufferMultisampleSupported())
        {
            Log(Debug::Warning) << "Post processing and reverse-z disabled. RenderBufferMultiSample unsupported, disabling antialiasing may resolve this issue.";
            return;
        }

        if (SceneUtil::AutoDepth::isReversed())
        {
            if (osg::isGLExtensionSupported(contextID, "GL_ARB_depth_buffer_float"))
                mDepthFormat = GL_DEPTH32F_STENCIL8;
            else if (osg::isGLExtensionSupported(contextID, "GL_NV_depth_buffer_float"))
                mDepthFormat = GL_DEPTH32F_STENCIL8_NV;
            else
                Log(Debug::Warning) << "Floating point depth buffer disabled, 'GL_ARB_depth_buffer_float' and 'GL_NV_depth_buffer_float' unsupported. reverse-z will not benefit the scene.";
        }

        mEnabled = true;
        mUsePostProcessing = usePostProcessing;
        mSoftParticles = softParticles;

#ifdef ANDROID
        mDisableDepthPasses = true;
#else
        mDisableDepthPasses = !usePostProcessing && !mSoftParticles;
#endif

        if (!mDisableDepthPasses)
        {
            mTransparentDepthPostPass = new TransparentDepthBinCallback(mRendering.getResourceSystem()->getSceneManager()->getShaderManager(), Settings::Manager::getBool("transparent postpass", "Post Processing"));
            osgUtil::RenderBin::getRenderBinPrototype("DepthSortedBin")->setDrawCallback(mTransparentDepthPostPass);
        }

        if (mUsePostProcessing)
        {
            for (const auto& name : mVFS->getRecursiveDirectoryIterator(fx::Technique::sSubdir))
            {
                std::filesystem::path path = name;
                std::string fileExt = Misc::StringUtils::lowerCase(path.extension().string());
                if (!path.parent_path().has_parent_path() && fileExt == fx::Technique::sExt)
                {
                    auto absolutePath = std::filesystem::path(mVFS->getAbsoluteFileName(name));

                    mTechniqueFileMap[absolutePath.stem().string()] = absolutePath;
                }
            }
        }

        mRootNode = new osg::Group;

        mMainTemplate->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mMainTemplate->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mMainTemplate->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mMainTemplate->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mMainTemplate->setInternalFormat(GL_RGBA);
        mMainTemplate->setSourceType(GL_UNSIGNED_BYTE);
        mMainTemplate->setSourceFormat(GL_RGBA);

        if (mSamples > 1)
        {
            mResolveCullCallback = new ResolveFboInterceptor;
            mRootNode->setCullCallback(mResolveCullCallback);
        }

        createTexturesAndCamera(width(), height());

        mRootNode->addChild(mHUDCamera);
        mRootNode->addChild(rootNode);

        mViewer->setSceneData(mRootNode);

        mViewer->getCamera()->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);

        // Not redundant! We change renderstage FBOs during cull traversals
        mViewer->getCamera()->setImplicitBufferAttachmentMask(0, 0);

        mViewer->getCamera()->getGraphicsContext()->setResizedCallback(new ResizedCallback(this));
        mViewer->getCamera()->setUserData(this);

        mRootNode->addCullCallback(mStateUpdater);
        mHUDCamera->setCullCallback(new SceneUtil::StateSetUpdater);

        mReload = true;
    }

    PostProcessor::~PostProcessor()
    {
        if (auto* bin = osgUtil::RenderBin::getRenderBinPrototype("DepthSortedBin"))
            bin->setDrawCallback(nullptr);
    }

    void PostProcessor::resize(int width, int height, bool resizeAttachments)
    {
        for (auto& technique : mTechniques)
        {
            for (auto& [name, rt] : technique->getRenderTargetsMap())
            {
                const auto [w, h] = rt.mSize.get(width, height);
                rt.mTarget->setTextureSize(w, h);
                rt.mTarget->dirtyTextureObject();
            }
        }

        if (resizeAttachments)
            createTexturesAndCamera(width, height);

        createObjectsForFrame(frame() % 2, width, height);

        mHUDCamera->resize(width, height);
        mViewer->getCamera()->resize(width, height);
        mRendering.updateProjectionMatrix();
        mRendering.setScreenRes(width, height);

        dirtyTechniques();

        mPingPongCanvas->dirty(frame());
        mDirty[(frame() + 1) % 2] = true;
    }

    void PostProcessor::update()
    {
        if (!mEnabled)
            return;

        bool needsReload = false;

        if (Settings::Manager::getBool("live reload", "Post Processing"))
        {
            for (auto& technique : mTechniques)
            {
                if (technique->getStatus() == fx::Technique::Status::File_Not_exists)
                    continue;

                technique->setLastModificationTime(std::filesystem::last_write_time(mTechniqueFileMap[technique->getName()]));

                if (technique->isDirty())
                    needsReload = true;
            }
        }

        if (needsReload)
        {
            for (auto& technique : mTechniques)
            {
                if (technique->isValid() && !technique->isDirty())
                    continue;

                try
                {
                    technique->compile();

                    reloadMainPass(*technique);
                    Log(Debug::Info) << "reloaded technique '" << technique->getFileName() << "'";
                }
                catch(const std::runtime_error& err)
                {
                    Log(Debug::Error) << "failed reloading technique file '" << technique->getFileName() << "': " << err.what();
                }
            }

            mReload = true;
        }

        if (mReload)
        {
            mReload = false;
            reloadTechniques();

            if (!mUsePostProcessing)
                resize(width(), height());
        }

        size_t frameId = frame() % 2;

        if (mDirty[frameId])
        {
            createObjectsForFrame(frameId, width(), height());
            mDirty[frameId] = false;
        }

        mPingPongCanvas->setMask(frameId, mUnderwater, mExteriorFlag);
        mPingPongCanvas->setHDR(frameId, getHDR());
        mPingPongCanvas->setFallbackFbo(frameId, getFbo(FBO_Primary, frameId));
        mPingPongCanvas->setSceneTexture(frameId, getTexture(Tex_Scene, frameId));
        mPingPongCanvas->setLDRSceneTexture(frameId, getTexture(Tex_Scene_LDR, frameId));

        if (mDisableDepthPasses)
            mPingPongCanvas->setDepthTexture(frameId, getTexture(Tex_Depth, frameId));
        else
            mPingPongCanvas->setDepthTexture(frameId, getTexture(Tex_OpaqueDepth, frameId));
    }

    void PostProcessor::createObjectsForFrame(size_t frameId, int width, int height)
    {
        auto& fbos = mFbos[frameId];
        auto& textures = mTextures[frameId];

        for (auto& tex : textures)
        {
            if (!tex)
                continue;

            tex->setTextureSize(width, height);
            tex->dirtyTextureObject();
        }

        fbos[FBO_Primary] = new osg::FrameBufferObject;
        fbos[FBO_Primary]->setAttachment(osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(textures[Tex_Scene]));
        fbos[FBO_Primary]->setAttachment(osg::Camera::PACKED_DEPTH_STENCIL_BUFFER, osg::FrameBufferAttachment(textures[Tex_Depth]));

        fbos[FBO_FirstPerson] = new osg::FrameBufferObject;
        osg::ref_ptr<osg::RenderBuffer> fpDepthRb = new osg::RenderBuffer(width, height, textures[Tex_Depth]->getInternalFormat(), mSamples > 1 ? mSamples : 0);
        fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER, osg::FrameBufferAttachment(fpDepthRb));

        // When MSAA is enabled we must first render to a render buffer, then
        // blit the result to the FBO which is either passed to the main frame
        // buffer for display or used as the entry point for a post process chain.
        if (mSamples > 1)
        {
            fbos[FBO_Multisample] = new osg::FrameBufferObject;
            osg::ref_ptr<osg::RenderBuffer> colorRB = new osg::RenderBuffer(width, height, textures[Tex_Scene]->getInternalFormat(), mSamples);
            osg::ref_ptr<osg::RenderBuffer> depthRB = new osg::RenderBuffer(width, height, textures[Tex_Depth]->getInternalFormat(), mSamples);
            fbos[FBO_Multisample]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(colorRB));
            fbos[FBO_Multisample]->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER, osg::FrameBufferAttachment(depthRB));
            fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(colorRB));
        }
        else
            fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(textures[Tex_Scene]));

        if (textures[Tex_OpaqueDepth])
        {
            fbos[FBO_OpaqueDepth] = new osg::FrameBufferObject;
            fbos[FBO_OpaqueDepth]->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER, osg::FrameBufferAttachment(textures[Tex_OpaqueDepth]));
        }

#ifdef __APPLE__
        if (textures[Tex_OpaqueDepth])
            fbos[FBO_OpaqueDepth]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER, osg::FrameBufferAttachment(new osg::RenderBuffer(textures[Tex_OpaqueDepth]->getTextureWidth(), textures[Tex_OpaqueDepth]->getTextureHeight(), textures[Tex_Scene]->getInternalFormat())));
#endif
    
        if (mResolveCullCallback)
            mResolveCullCallback->setFbo(frameId, mFbos[frameId][FBO_Primary]);

        if (mTransparentDepthPostPass)
        {
            mTransparentDepthPostPass->mFbo[frameId] = mFbos[frameId][FBO_Primary];
            mTransparentDepthPostPass->mMsaaFbo[frameId] = mFbos[frameId][FBO_Multisample];
            mTransparentDepthPostPass->mOpaqueFbo[frameId] = mFbos[frameId][FBO_OpaqueDepth];
        }
    }

    void PostProcessor::dirtyTechniques()
    {
        if (!isEnabled())
            return;

        fx::DispatchArray data;

        bool sunglare = true;
        mHDR = false;

        for (const auto& technique : mTechniques)
        {
            if (!technique->isValid())
                continue;

            fx::DispatchNode node;

            node.mFlags = technique->getFlags();

            if (technique->getHDR())
                mHDR = true;

            if (node.mFlags & fx::Technique::Flag_Disable_SunGlare)
                sunglare = false;

            // required default samplers available to every shader pass
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerLastShader", Unit_LastShader));
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerLastPass", Unit_LastPass));
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerDepth", Unit_Depth));

            if (technique->getHDR())
                node.mRootStateSet->addUniform(new osg::Uniform("omw_EyeAdaptation", Unit_EyeAdaptation));

            int texUnit = Unit_NextFree;

            // user-defined samplers
            for (const osg::Texture* texture : technique->getTextures())
            {
                if (const auto* tex1D = dynamic_cast<const osg::Texture1D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture1D(*tex1D));
                else if (const auto* tex2D = dynamic_cast<const osg::Texture2D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture2D(*tex2D));
                else if (const auto* tex3D = dynamic_cast<const osg::Texture3D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture3D(*tex3D));

                node.mRootStateSet->addUniform(new osg::Uniform(texture->getName().c_str(), texUnit++));
            }

            // user-defined uniforms
            for (auto& uniform : technique->getUniformMap())
            {
                if (uniform->mSamplerType) continue;

                if (auto type = uniform->getType())
                    uniform->setUniform(node.mRootStateSet->getOrCreateUniform(uniform->mName, type.value()));
            }

            int subTexUnit = texUnit;

            for (const auto& pass : technique->getPasses())
            {
                fx::DispatchNode::SubPass subPass;

                pass->prepareStateSet(subPass.mStateSet, technique->getName());

                node.mHandle = technique;

                if (!pass->getTarget().empty())
                {
                    const auto& rt = technique->getRenderTargetsMap()[pass->getTarget()];

                    const auto [w, h] = rt.mSize.get(width(), height());

                    subPass.mRenderTexture = new osg::Texture2D(*rt.mTarget);
                    subPass.mRenderTexture->setTextureSize(w, h);
                    subPass.mRenderTexture->setName(std::string(pass->getTarget()));

                    if (rt.mMipMap)
                        subPass.mRenderTexture->setNumMipmapLevels(osg::Image::computeNumberOfMipmapLevels(w, h));

                    subPass.mRenderTarget = new osg::FrameBufferObject;
                    subPass.mRenderTarget->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(subPass.mRenderTexture));
                    subPass.mStateSet->setAttributeAndModes(new osg::Viewport(0, 0, w, h));

                    node.mRootStateSet->setTextureAttributeAndModes(subTexUnit, subPass.mRenderTexture);
                    node.mRootStateSet->addUniform(new osg::Uniform(subPass.mRenderTexture->getName().c_str(), subTexUnit++));
                }
                node.mPasses.emplace_back(std::move(subPass));
            }

            data.emplace_back(std::move(node));
        }

        mPingPongCanvas->setCurrentFrameData(frame() % 2, std::move(data));

        if (auto hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())
            hud->updateTechniques();

        mRendering.getSkyManager()->setSunglare(sunglare);
    }

    bool PostProcessor::enableTechnique(std::shared_ptr<fx::Technique> technique, std::optional<int> location)
    {
        if (!technique || technique->getName() == "main" || (location.has_value() && location.value() <= 0))
            return false;

        disableTechnique(technique, false);

        int pos = std::min<int>(location.value_or(mTechniques.size()), mTechniques.size());

        mTechniques.insert(mTechniques.begin() + pos, technique);
        dirtyTechniques();

        return true;
    }

    bool PostProcessor::disableTechnique(std::shared_ptr<fx::Technique> technique, bool dirty)
    {
        for (size_t i = 1; i < mTechniques.size(); ++i)
        {
            if (technique.get() == mTechniques[i].get())
            {
                mTechniques.erase(mTechniques.begin() + i);
                if (dirty)
                    dirtyTechniques();
                return true;
            }
        }

        return false;
    }

    bool PostProcessor::isTechniqueEnabled(const std::shared_ptr<fx::Technique>& technique) const
    {
        for (const auto& t : mTechniques)
        {
            if (technique.get() == t.get())
                return technique->isValid();
        }

        return false;
    }

    void PostProcessor::createTexturesAndCamera(int width, int height)
    {
        for (auto& textures : mTextures)
        {
            for (auto& texture : textures)
            {
                if (!texture)
                    texture = new osg::Texture2D;
                texture->setTextureSize(width, height);
                texture->setSourceFormat(GL_RGBA);
                texture->setSourceType(GL_UNSIGNED_BYTE);
                texture->setInternalFormat(GL_RGBA);
                texture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture::LINEAR);
                texture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture::LINEAR);
                texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
                texture->setResizeNonPowerOfTwoHint(false);
            }

            if (mMainTemplate)
            {
                textures[Tex_Scene]->setSourceFormat(mMainTemplate->getSourceFormat());
                textures[Tex_Scene]->setSourceType(mMainTemplate->getSourceType());
                textures[Tex_Scene]->setInternalFormat(mMainTemplate->getInternalFormat());
                textures[Tex_Scene]->setFilter(osg::Texture2D::MIN_FILTER, mMainTemplate->getFilter(osg::Texture2D::MIN_FILTER));
                textures[Tex_Scene]->setFilter(osg::Texture2D::MAG_FILTER, mMainTemplate->getFilter(osg::Texture2D::MAG_FILTER));
                textures[Tex_Scene]->setWrap(osg::Texture::WRAP_S, mMainTemplate->getWrap(osg::Texture2D::WRAP_S));
                textures[Tex_Scene]->setWrap(osg::Texture::WRAP_T, mMainTemplate->getWrap(osg::Texture2D::WRAP_T));
            }

            auto setupDepth = [this] (osg::Texture2D* tex) {
                tex->setSourceFormat(GL_DEPTH_STENCIL_EXT);
                tex->setSourceType(SceneUtil::isFloatingPointDepthFormat(omw_GetDepthFormat()) ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8_EXT);
                tex->setInternalFormat(mDepthFormat);
            };

            setupDepth(textures[Tex_Depth]);

            if (mDisableDepthPasses)
            {
                textures[Tex_OpaqueDepth] = nullptr;
            }
            else
            {
                setupDepth(textures[Tex_OpaqueDepth]);
                textures[Tex_OpaqueDepth]->setName("opaqueTexMap");
            }
        }

        if (mHUDCamera)
            return;

        mHUDCamera = new osg::Camera;
        mHUDCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        mHUDCamera->setRenderOrder(osg::Camera::POST_RENDER);
        mHUDCamera->setClearColor(osg::Vec4(0.45, 0.45, 0.14, 1.0));
        mHUDCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, 1, 0, 1));
        mHUDCamera->setAllowEventFocus(false);
        mHUDCamera->setViewport(0, 0, width, height);

        mPingPongCull = new PingPongCull;
        mViewer->getCamera()->addCullCallback(mPingPongCull);

        mPingPongCanvas = new PingPongCanvas(mUsePostProcessing, mRendering.getResourceSystem()->getSceneManager()->getShaderManager());

        mHUDCamera->addChild(mPingPongCanvas);
        mHUDCamera->setNodeMask(Mask_RenderToTexture);

        mHUDCamera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        mHUDCamera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
    }

    std::shared_ptr<fx::Technique> PostProcessor::loadTechnique(const std::string& name, bool insert)
    {
        if (!isEnabled())
            return nullptr;

        for (size_t i = 0; i < mTemplates.size(); ++i)
            if (name == mTemplates[i]->getName())
                return mTemplates[i];

        auto technique = std::make_shared<fx::Technique>(*mVFS, *mRendering.getResourceSystem()->getImageManager(), name, width(), height(), mUBO);

        technique->compile();

        if (technique->getStatus() != fx::Technique::Status::File_Not_exists)
            technique->setLastModificationTime(std::filesystem::last_write_time(mTechniqueFileMap[technique->getName()]), false);

        if (!insert)
            return technique;

        reloadMainPass(*technique);

        mTemplates.push_back(std::move(technique));

        return mTemplates.back();
    }

    void PostProcessor::addTemplate(std::shared_ptr<fx::Technique> technique)
    {
        if (!isEnabled())
            return;

        for (size_t i = 0; i < mTemplates.size(); ++i)
            if (technique.get() == mTemplates[i].get())
                return;

        mTemplates.push_back(technique);
    }

    void PostProcessor::reloadTechniques()
    {
        if (!isEnabled())
            return;

        mTechniques.clear();

        std::vector<std::string> techniqueStrings;
        Misc::StringUtils::split(Settings::Manager::getString("chain", "Post Processing"), techniqueStrings, ",");

        techniqueStrings.insert(techniqueStrings.begin(), "main");

        for (auto& techniqueName : techniqueStrings)
        {
            Misc::StringUtils::trim(techniqueName);

            if (techniqueName.empty())
                continue;

            if ((&techniqueName != &techniqueStrings.front()) && Misc::StringUtils::ciEqual(techniqueName, "main"))
            {
                Log(Debug::Warning) << "main.omwfx techniqued specified in chain, this is not allowed. technique file will be ignored if it exists.";
                continue;
            }

            mTechniques.push_back(loadTechnique(techniqueName));
        }

        dirtyTechniques();
    }

    void PostProcessor::reloadMainPass(fx::Technique& technique)
    {
        if (!technique.getMainTemplate())
            return;

        auto mainTemplate = technique.getMainTemplate();

        if (mMainTemplate->getSourceFormat() == mainTemplate->getSourceFormat() &&
            mMainTemplate->getSourceType() == mainTemplate->getSourceType() &&
            mMainTemplate->getInternalFormat() == mainTemplate->getInternalFormat() &&
            mMainTemplate->getFilter(osg::Texture::MIN_FILTER) == mainTemplate->getFilter(osg::Texture::MIN_FILTER) &&
            mMainTemplate->getFilter(osg::Texture::MAG_FILTER) == mainTemplate->getFilter(osg::Texture::MAG_FILTER) &&
            mMainTemplate->getWrap(osg::Texture::WRAP_S) == mainTemplate->getWrap(osg::Texture::WRAP_S) &&
            mMainTemplate->getWrap(osg::Texture::WRAP_T) == mainTemplate->getWrap(osg::Texture::WRAP_T) &&
            technique.getHDR() == mHDR
        )
            return;

        mMainTemplate = mainTemplate;

        resize(width(), height(), true);
    }

    void PostProcessor::toggleMode()
    {
        for (auto& technique : mTemplates)
            technique->compile();

        dirtyTechniques();
    }
}
