#include <spokk.h>
using namespace spokk;

#include <common/camera.h>
#include <common/cube_mesh.h>

#include <mathfu/glsl_mappings.h>
#include <mathfu/vector.h>

#include <array>
#include <cstdio>
#include <memory>

namespace {
struct SceneUniforms {
  mathfu::vec4_packed time_and_res;  // x: elapsed seconds, yz: viewport resolution in pixels
  mathfu::vec4_packed eye_pos_ws;  // xyz: world-space eye position
  mathfu::vec4_packed eye_dir_wsn;  // xyz: world-space eye direction (normalized)
  mathfu::mat4 viewproj;
  mathfu::mat4 view;
  mathfu::mat4 proj;
  mathfu::mat4 viewproj_inv;
  mathfu::mat4 view_inv;
  mathfu::mat4 proj_inv;
};
constexpr float FOV_DEGREES = 45.0f;
constexpr float Z_NEAR = 0.01f;
constexpr float Z_FAR = 100.0f;
}  // namespace

class LightsApp : public spokk::Application {
public:
  explicit LightsApp(Application::CreateInfo& ci) : Application(ci) {
    glfwSetInputMode(window_.get(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    seconds_elapsed_ = 0;

    camera_ =
        my_make_unique<CameraPersp>(swapchain_extent_.width, swapchain_extent_.height, FOV_DEGREES, Z_NEAR, Z_FAR);
    const mathfu::vec3 initial_camera_pos(-1, 0, 6);
    const mathfu::vec3 initial_camera_target(0, 0, 0);
    const mathfu::vec3 initial_camera_up(0, 1, 0);
    camera_->lookAt(initial_camera_pos, initial_camera_target, initial_camera_up);
    dolly_ = my_make_unique<CameraDolly>(*camera_);

    // Create render pass
    render_pass_.InitFromPreset(RenderPass::Preset::COLOR_DEPTH, swapchain_surface_format_.format);
    SPOKK_VK_CHECK(render_pass_.Finalize(device_context_));
    render_pass_.clear_values[0] = CreateColorClearValue(0.2f, 0.2f, 0.3f);
    render_pass_.clear_values[1] = CreateDepthClearValue(1.0f, 0);

    // Load textures and samplers
    VkSamplerCreateInfo sampler_ci =
        GetSamplerCreateInfo(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    SPOKK_VK_CHECK(vkCreateSampler(device_, &sampler_ci, host_allocator_, &sampler_));
    int load_error = skybox_tex_.CreateFromFile(device_context_, graphics_and_present_queue_, "data/testcube.ktx");
    ZOMBO_ASSERT(load_error == 0, "texture load error (%d)", load_error);

    // Load shaders (forcing compatible pipeline layouts)
    SPOKK_VK_CHECK(skybox_vs_.CreateAndLoadSpirvFile(device_context_, "skybox.vert.spv"));
    SPOKK_VK_CHECK(skybox_fs_.CreateAndLoadSpirvFile(device_context_, "skybox.frag.spv"));
    SPOKK_VK_CHECK(skybox_shader_program_.AddShader(&skybox_vs_));
    SPOKK_VK_CHECK(skybox_shader_program_.AddShader(&skybox_fs_));
    SPOKK_VK_CHECK(mesh_vs_.CreateAndLoadSpirvFile(device_context_, "lit_mesh.vert.spv"));
    SPOKK_VK_CHECK(mesh_fs_.CreateAndLoadSpirvFile(device_context_, "lit_mesh.frag.spv"));
    SPOKK_VK_CHECK(mesh_shader_program_.AddShader(&mesh_vs_));
    SPOKK_VK_CHECK(mesh_shader_program_.AddShader(&mesh_fs_));
    SPOKK_VK_CHECK(ShaderProgram::ForceCompatibleLayoutsAndFinalize(
        device_context_, {&skybox_shader_program_, &mesh_shader_program_}));

    // Create skybox pipeline
    empty_mesh_format_.Finalize(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    skybox_pipeline_.Init(&empty_mesh_format_, &skybox_shader_program_, &render_pass_, 0);
    skybox_pipeline_.depth_stencil_state_ci.depthWriteEnable = VK_FALSE;
    skybox_pipeline_.depth_stencil_state_ci.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    SPOKK_VK_CHECK(skybox_pipeline_.Finalize(device_context_));

    // Populate Mesh object
    int mesh_load_error = mesh_.CreateFromFile(device_context_, "data/teapot.mesh");
    ZOMBO_ASSERT(!mesh_load_error, "Error loading mesh");

    // Create mesh pipeline
    mesh_pipeline_.Init(&mesh_.mesh_format, &mesh_shader_program_, &render_pass_, 0);
    SPOKK_VK_CHECK(mesh_pipeline_.Finalize(device_context_));

    // Create pipelined buffer of mesh uniforms
    VkBufferCreateInfo mesh_uniforms_ci = {};
    mesh_uniforms_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    mesh_uniforms_ci.size = sizeof(mathfu::mat4);
    mesh_uniforms_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    mesh_uniforms_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    SPOKK_VK_CHECK(mesh_uniforms_.Create(device_context_, PFRAME_COUNT, mesh_uniforms_ci));

    // Create pipelined buffer of shader uniforms
    VkBufferCreateInfo scene_uniforms_ci = {};
    scene_uniforms_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    scene_uniforms_ci.size = sizeof(SceneUniforms);
    scene_uniforms_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    scene_uniforms_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    SPOKK_VK_CHECK(
        scene_uniforms_.Create(device_context_, PFRAME_COUNT, scene_uniforms_ci, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));

    for (const auto& dset_layout_ci : skybox_shader_program_.dset_layout_cis) {
      dpool_.Add(dset_layout_ci, PFRAME_COUNT);
    }
    SPOKK_VK_CHECK(dpool_.Finalize(device_context_));
    for (uint32_t pframe = 0; pframe < PFRAME_COUNT; ++pframe) {
      // TODO(cort): allocate_pipelined_set()?
      dsets_[pframe] = dpool_.AllocateSet(device_context_, skybox_shader_program_.dset_layouts[0]);
    }
    DescriptorSetWriter dset_writer(skybox_shader_program_.dset_layout_cis[0]);
    dset_writer.BindImage(skybox_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        skybox_fs_.GetDescriptorBindPoint("skybox_tex").binding);
    dset_writer.BindSampler(sampler_, skybox_fs_.GetDescriptorBindPoint("skybox_samp").binding);
    for (uint32_t pframe = 0; pframe < PFRAME_COUNT; ++pframe) {
      dset_writer.BindBuffer(scene_uniforms_.Handle(pframe), mesh_vs_.GetDescriptorBindPoint("scene_consts").binding);
      dset_writer.BindBuffer(mesh_uniforms_.Handle(pframe), mesh_vs_.GetDescriptorBindPoint("mesh_consts").binding);
      dset_writer.WriteAll(device_context_, dsets_[pframe]);
    }

    // Create swapchain-sized buffers
    CreateRenderBuffers(swapchain_extent_);
  }
  virtual ~LightsApp() {
    if (device_) {
      vkDeviceWaitIdle(device_);

      dpool_.Destroy(device_context_);

      mesh_uniforms_.Destroy(device_context_);
      scene_uniforms_.Destroy(device_context_);

      mesh_vs_.Destroy(device_context_);
      mesh_fs_.Destroy(device_context_);
      mesh_shader_program_.Destroy(device_context_);
      mesh_pipeline_.Destroy(device_context_);
      mesh_.Destroy(device_context_);

      skybox_vs_.Destroy(device_context_);
      skybox_fs_.Destroy(device_context_);
      skybox_shader_program_.Destroy(device_context_);
      skybox_pipeline_.Destroy(device_context_);

      vkDestroySampler(device_, sampler_, host_allocator_);
      skybox_tex_.Destroy(device_context_);

      for (const auto fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, host_allocator_);
      }
      render_pass_.Destroy(device_context_);

      depth_image_.Destroy(device_context_);
    }
  }

  LightsApp(const LightsApp&) = delete;
  const LightsApp& operator=(const LightsApp&) = delete;

  virtual void Update(double dt) override {
    Application::Update(dt);
    seconds_elapsed_ += dt;

    // Update camera
    mathfu::vec3 impulse(0, 0, 0);
    const float MOVE_SPEED = 0.5f, TURN_SPEED = 0.001f;
    if (input_state_.GetDigital(InputState::DIGITAL_LPAD_UP)) {
      impulse += camera_->getViewDirection() * MOVE_SPEED;
    }
    if (input_state_.GetDigital(InputState::DIGITAL_LPAD_LEFT)) {
      mathfu::vec3 viewRight = camera_->getOrientation() * mathfu::vec3(1, 0, 0);
      impulse -= viewRight * MOVE_SPEED;
    }
    if (input_state_.GetDigital(InputState::DIGITAL_LPAD_DOWN)) {
      impulse -= camera_->getViewDirection() * MOVE_SPEED;
    }
    if (input_state_.GetDigital(InputState::DIGITAL_LPAD_RIGHT)) {
      mathfu::vec3 viewRight = camera_->getOrientation() * mathfu::vec3(1, 0, 0);
      impulse += viewRight * MOVE_SPEED;
    }
    if (input_state_.GetDigital(InputState::DIGITAL_RPAD_DOWN)) {
      mathfu::vec3 viewUp = camera_->getOrientation() * mathfu::vec3(0, 1, 0);
      impulse += viewUp * MOVE_SPEED;
    }

    // Update camera based on mouse delta
    mathfu::vec3 camera_eulers =
        camera_->getEulersYPR() + mathfu::vec3(-TURN_SPEED * input_state_.GetAnalogDelta(InputState::ANALOG_MOUSE_Y),
                                      -TURN_SPEED * input_state_.GetAnalogDelta(InputState::ANALOG_MOUSE_X), 0);
    if (camera_eulers[0] >= float(M_PI_2 - 0.01f)) {
      camera_eulers[0] = float(M_PI_2 - 0.01f);
    } else if (camera_eulers[0] <= float(-M_PI_2 + 0.01f)) {
      camera_eulers[0] = float(-M_PI_2 + 0.01f);
    }
    camera_eulers[2] = 0;  // disallow roll
    camera_->setOrientation(mathfu::quat::FromEulerAngles(camera_eulers));
    dolly_->Impulse(impulse);
    dolly_->Update((float)dt);

    // Update uniforms
    SceneUniforms* uniforms = (SceneUniforms*)scene_uniforms_.Mapped(pframe_index_);
    uniforms->time_and_res =
        mathfu::vec4((float)seconds_elapsed_, (float)swapchain_extent_.width, (float)swapchain_extent_.height, 0);
    uniforms->eye_pos_ws = mathfu::vec4(camera_->getEyePoint(), 1.0f);
    uniforms->eye_dir_wsn = mathfu::vec4(camera_->getViewDirection().Normalized(), 1.0f);
    const mathfu::mat4 view = camera_->getViewMatrix();
    const mathfu::mat4 proj = camera_->getProjectionMatrix();
    // clang-format off
    const mathfu::mat4 clip_fixup(
      +1.0f, +0.0f, +0.0f, +0.0f,
      +0.0f, -1.0f, +0.0f, +0.0f,
      +0.0f, +0.0f, +0.5f, +0.5f,
      +0.0f, +0.0f, +0.0f, +1.0f);
    // clang-format on
    const mathfu::mat4 viewproj = (clip_fixup * proj) * view;
    uniforms->viewproj = viewproj;
    uniforms->view = view;
    uniforms->proj = clip_fixup * proj;
    uniforms->viewproj_inv = viewproj.Inverse();
    uniforms->view_inv = view.Inverse();
    uniforms->proj_inv = (clip_fixup * proj).Inverse();
    scene_uniforms_.FlushPframeHostCache(pframe_index_);

    // Update mesh uniforms
    // clang-format off
    mathfu::quat q = mathfu::quat::identity;
    mathfu::mat4 o2w = mathfu::mat4::Identity()
      * mathfu::mat4::FromTranslationVector(mathfu::vec3(0.0f, 0.0f, 0.0f))
      * q.ToMatrix4()
      * mathfu::mat4::FromScaleVector( mathfu::vec3(5.0f, 5.0f, 5.0f) )
      ;
    // clang-format on
    mesh_uniforms_.Load(device_context_, pframe_index_, &o2w, sizeof(mathfu::mat4), 0, 0);
  }

  void Render(VkCommandBuffer primary_cb, uint32_t swapchain_image_index) override {
    VkFramebuffer framebuffer = framebuffers_[swapchain_image_index];
    render_pass_.begin_info.framebuffer = framebuffer;
    render_pass_.begin_info.renderArea.extent = swapchain_extent_;
    vkCmdBeginRenderPass(primary_cb, &render_pass_.begin_info, VK_SUBPASS_CONTENTS_INLINE);
    // Set up shared render state
    VkRect2D scissor_rect = render_pass_.begin_info.renderArea;
    VkViewport viewport = Rect2DToViewport(scissor_rect);
    vkCmdSetViewport(primary_cb, 0, 1, &viewport);
    vkCmdSetScissor(primary_cb, 0, 1, &scissor_rect);
    vkCmdBindDescriptorSets(primary_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_.shader_program->pipeline_layout,
        0, 1, &dsets_[pframe_index_], 0, nullptr);
    // Render scene
    vkCmdBindPipeline(primary_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_.handle);
    mesh_.BindBuffersAndDraw(primary_cb, mesh_.index_count);
    // Render skybox
    vkCmdBindPipeline(primary_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_.handle);
    vkCmdDraw(primary_cb, 36, 1, 0, 0);

    vkCmdEndRenderPass(primary_cb);
  }

protected:
  void HandleWindowResize(VkExtent2D new_window_extent) override {
    Application::HandleWindowResize(new_window_extent);

    // Destroy existing objects before re-creating them.
    for (auto fb : framebuffers_) {
      if (fb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, fb, host_allocator_);
      }
    }
    framebuffers_.clear();
    depth_image_.Destroy(device_context_);

    float aspect_ratio = (float)new_window_extent.width / (float)new_window_extent.height;
    camera_->setPerspective(FOV_DEGREES, aspect_ratio, Z_NEAR, Z_FAR);

    CreateRenderBuffers(new_window_extent);
  }

private:
  void CreateRenderBuffers(VkExtent2D extent) {
    // Create depth buffer
    VkImageCreateInfo depth_image_ci = render_pass_.GetAttachmentImageCreateInfo(1, extent);
    depth_image_ = {};
    SPOKK_VK_CHECK(depth_image_.Create(
        device_context_, depth_image_ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DEVICE_ALLOCATION_SCOPE_DEVICE));

    // Create VkFramebuffers
    std::vector<VkImageView> attachment_views = {
        VK_NULL_HANDLE,  // filled in below
        depth_image_.view,
    };
    VkFramebufferCreateInfo framebuffer_ci = render_pass_.GetFramebufferCreateInfo(extent);
    framebuffer_ci.pAttachments = attachment_views.data();
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
      attachment_views[0] = swapchain_image_views_[i];
      SPOKK_VK_CHECK(vkCreateFramebuffer(device_, &framebuffer_ci, host_allocator_, &framebuffers_[i]));
    }
  }

  double seconds_elapsed_;

  Image depth_image_;

  RenderPass render_pass_;
  std::vector<VkFramebuffer> framebuffers_;

  Image skybox_tex_;
  VkSampler sampler_;

  Shader skybox_vs_, skybox_fs_;
  ShaderProgram skybox_shader_program_;
  GraphicsPipeline skybox_pipeline_;
  MeshFormat empty_mesh_format_;

  DescriptorPool dpool_;
  std::array<VkDescriptorSet, PFRAME_COUNT> dsets_;

  Shader mesh_vs_, mesh_fs_;
  ShaderProgram mesh_shader_program_;
  GraphicsPipeline mesh_pipeline_;
  Mesh mesh_;
  PipelinedBuffer mesh_uniforms_;
  PipelinedBuffer scene_uniforms_;

  std::unique_ptr<CameraPersp> camera_;
  std::unique_ptr<CameraDolly> dolly_;
};

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  std::vector<Application::QueueFamilyRequest> queue_requests = {
      {(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT), true, 1, 0.0f}};
  Application::CreateInfo app_ci = {};
  app_ci.queue_family_requests = queue_requests;
  app_ci.pfn_set_device_features = EnableMinimumDeviceFeatures;

  LightsApp app(app_ci);
  int run_error = app.Run();

  return run_error;
}