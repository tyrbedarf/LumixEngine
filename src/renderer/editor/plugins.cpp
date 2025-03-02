#include "editor/asset_browser.h"
#include "editor/platform_interface.h"
#include "editor/property_grid.h"
#include "editor/render_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/fs/disk_file_device.h"
#include "engine/fs/file_system.h"
#include "engine/fs/os_file.h"
#include "engine/job_system.h"
#include "engine/json_serializer.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/lumix.h"
#include "engine/mt/atomic.h"
#include "engine/mt/task.h"
#include "engine/mt/thread.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/prefab.h"
#include "engine/reflection.h"
#include "engine/queue.h"
#include "engine/resource_manager.h"
#include "engine/resource_manager_base.h"
#include "engine/system.h"
#include "engine/universe/universe.h"
#include "game_view.h"
#include "import_asset_dialog.h"
#include "renderer/draw2d.h"
#include "renderer/font_manager.h"
#include "renderer/frame_buffer.h"
#include "renderer/material.h"
#include "renderer/model.h"
#include "renderer/particle_system.h"
#include "renderer/pipeline.h"
#include "renderer/render_scene.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "scene_view.h"
#include "shader_compiler.h"
#include "shader_editor.h"
#include "stb/stb_image.h"
#include "stb/stb_image_resize.h"
#include "terrain_editor.h"
#include <SDL.h>
#include <cmath>
#include <cmft/clcontext.h>
#include <cmft/cubemapfilter.h>
#include <crnlib.h>
#include <cstddef>


using namespace Lumix;


static const ComponentType PARTICLE_EMITTER_TYPE = Reflection::getComponentType("particle_emitter");
static const ComponentType SCRIPTED_PARTICLE_EMITTER_TYPE = Reflection::getComponentType("scripted_particle_emitter");
static const ComponentType TERRAIN_TYPE = Reflection::getComponentType("terrain");
static const ComponentType CAMERA_TYPE = Reflection::getComponentType("camera");
static const ComponentType DECAL_TYPE = Reflection::getComponentType("decal");
static const ComponentType POINT_LIGHT_TYPE = Reflection::getComponentType("point_light");
static const ComponentType GLOBAL_LIGHT_TYPE = Reflection::getComponentType("global_light");
static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("renderable");
static const ComponentType TEXT_MESH_TYPE = Reflection::getComponentType("text_mesh");
static const ComponentType ENVIRONMENT_PROBE_TYPE = Reflection::getComponentType("environment_probe");


struct FontPlugin LUMIX_FINAL : public AssetBrowser::IPlugin
{
	FontPlugin(StudioApp& app)
	{
		app.getAssetBrowser().registerExtension("ttf", FontResource::TYPE);
	}

	void onGUI(Resource* resource) override {}
	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Font"; }

	ResourceType getResourceType() const override { return FontResource::TYPE; }
};


struct MaterialPlugin LUMIX_FINAL : public AssetBrowser::IPlugin
{
	explicit MaterialPlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetBrowser().registerExtension("mat", Material::TYPE);
	}


	void saveMaterial(Material* material)
	{
		if (FS::IFile* file = m_app.getAssetBrowser().beginSaveResource(*material))
		{
			JsonSerializer serializer(*file, material->getPath());
			bool success = true;
			if (!material->save(serializer))
			{
				success = false;
				g_log_error.log("Editor") << "Could not save file " << material->getPath().c_str();
			}
			m_app.getAssetBrowser().endSaveResource(*material, *file, success);
		}
	}


	void onGUI(Resource* resource) override
	{
		auto* material = static_cast<Material*>(resource);

		if (ImGui::Button("Save")) saveMaterial(material);
		ImGui::SameLine();
		if (ImGui::Button("Open in external editor")) m_app.getAssetBrowser().openInExternalEditor(material);

		auto* plugin = m_app.getWorldEditor().getEngine().getPluginManager().getPlugin("renderer");
		auto* renderer = static_cast<Renderer*>(plugin);

		int alpha_cutout_define = renderer->getShaderDefineIdx("ALPHA_CUTOUT");
		
		int render_layer = material->getRenderLayer();
		auto getter = [](void* data, int idx, const char** out) -> bool {
			auto* renderer = (Renderer*)data;
			*out = renderer->getLayerName(idx);
			return true;
		};
		if (ImGui::Combo("Render Layer", &render_layer, getter, renderer, renderer->getLayersCount()))
		{
			material->setRenderLayer(render_layer);
		}

		bool b = material->isBackfaceCulling();
		if (ImGui::Checkbox("Backface culling", &b)) material->enableBackfaceCulling(b);

		if (material->hasDefine(alpha_cutout_define))
		{
			b = material->isDefined(alpha_cutout_define);
			if (ImGui::Checkbox("Is alpha cutout", &b)) material->setDefine(alpha_cutout_define, b);
			if(b)
			{
				float tmp = material->getAlphaRef();
				if(ImGui::DragFloat("Alpha reference value", &tmp, 0.01f, 0.0f, 1.0f))
				{
					material->setAlphaRef(tmp);
				}
			}
		}

		Vec4 color = material->getColor();
		if (ImGui::ColorEdit4("Color", &color.x))
		{
			material->setColor(color);
		}

		float roughness = material->getRoughness();
		if (ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
		{
			material->setRoughness(roughness);
		}

		float metallic = material->getMetallic();
		if (ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
		{
			material->setMetallic(metallic);
		}

		float emission = material->getEmission();
		if (ImGui::DragFloat("Emission", &emission, 0.01f, 0.0f))
		{
			material->setEmission(emission);
		}

		char buf[MAX_PATH_LENGTH];
		copyString(buf, material->getShader() ? material->getShader()->getPath().c_str() : "");
		if (m_app.getAssetBrowser().resourceInput("Shader", "shader", buf, sizeof(buf), Shader::TYPE))
		{
			material->setShader(Path(buf));
		}

		for (int i = 0; i < material->getShader()->m_texture_slot_count; ++i)
		{
			auto& slot = material->getShader()->m_texture_slots[i];
			Texture* texture = material->getTexture(i);
			copyString(buf, texture ? texture->getPath().c_str() : "");
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
			bool is_node_open = ImGui::TreeNodeEx((const void*)(intptr_t)i, ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed, "%s", "");
			ImGui::PopStyleColor(4);
			ImGui::SameLine();
			if (m_app.getAssetBrowser().resourceInput(slot.name, StaticString<30>("", (u64)&slot), buf, sizeof(buf), Texture::TYPE))
			{
				material->setTexturePath(i, Path(buf));
			}
			if (!texture && is_node_open)
			{
				ImGui::TreePop();
				continue;
			}

			if(is_node_open)
			{
				ImGui::Image(&texture->handle, ImVec2(96, 96));

				if (ImGui::CollapsingHeader("Advanced"))
				{
					static const struct { const char* name; u32 value; u32 unset_flag; } FLAGS[] = {
						{"SRGB", BGFX_TEXTURE_SRGB, 0},
						{"u clamp", BGFX_TEXTURE_U_CLAMP, 0},
						{"v clamp", BGFX_TEXTURE_V_CLAMP, 0},
						{"Min point", BGFX_TEXTURE_MIN_POINT, BGFX_TEXTURE_MIN_ANISOTROPIC},
						{"Mag point", BGFX_TEXTURE_MAG_POINT, BGFX_TEXTURE_MAG_ANISOTROPIC},
						{"Min anisotropic", BGFX_TEXTURE_MIN_ANISOTROPIC, BGFX_TEXTURE_MIN_POINT},
						{"Mag anisotropic", BGFX_TEXTURE_MAG_ANISOTROPIC, BGFX_TEXTURE_MAG_POINT} };

					for (int i = 0; i < lengthOf(FLAGS); ++i)
					{
						auto& flag = FLAGS[i];
						bool b = (texture->bgfx_flags & flag.value) != 0;
						if (ImGui::Checkbox(flag.name, &b))
						{
							if (flag.unset_flag) texture->setFlag(flag.unset_flag, false);
							texture->setFlag(flag.value, b);
						}
					}
				}
				ImGui::TreePop();
			}
		}

		auto* shader = material->getShader();
		if(shader && material->isReady())
		{
			for(int i = 0; i < shader->m_uniforms.size(); ++i)
			{
				auto& uniform = material->getUniform(i);
				auto& shader_uniform = shader->m_uniforms[i];
				switch (shader_uniform.type)
				{
					case Shader::Uniform::FLOAT:
						if (ImGui::DragFloat(shader_uniform.name, &uniform.float_value))
						{
							material->createCommandBuffer();
						}
						break;
					case Shader::Uniform::VEC3:
						if (ImGui::DragFloat3(shader_uniform.name, uniform.vec3))
						{
							material->createCommandBuffer();
						}
						break;
					case Shader::Uniform::VEC4:
						if (ImGui::DragFloat4(shader_uniform.name, uniform.vec4))
						{
							material->createCommandBuffer();
						}
						break;
					case Shader::Uniform::VEC2:
						if (ImGui::DragFloat2(shader_uniform.name, uniform.vec2))
						{
							material->createCommandBuffer();
						}
						break;
					case Shader::Uniform::COLOR:
						if (ImGui::ColorEdit3(shader_uniform.name, uniform.vec3))
						{
							material->createCommandBuffer();
						}
						break;
					case Shader::Uniform::TIME: break;
					default: ASSERT(false); break;
				}
			}

			int layers_count = material->getLayersCount();
			if (ImGui::DragInt("Layers count", &layers_count, 1, 0, 256))
			{
				material->setLayersCount(layers_count);
			}
			
			if (ImGui::CollapsingHeader("Defines"))
			{
				for (int define_idx = 0; define_idx < renderer->getShaderDefinesCount(); ++define_idx)
				{
					const char* define = renderer->getShaderDefine(define_idx);
					if (!material->hasDefine(define_idx)) continue;
					bool value = material->isDefined(define_idx);

					auto isBuiltinDefine = [](const char* define) {
						const char* BUILTIN_DEFINES[] = {"HAS_SHADOWMAP", "ALPHA_CUTOUT", "SKINNED"};
						for (const char* builtin_define : BUILTIN_DEFINES)
						{
							if (equalStrings(builtin_define, define)) return true;
						}
						return false;
					};

					bool is_texture_define = material->isTextureDefine(define_idx);
					if (!is_texture_define && !isBuiltinDefine(define) && ImGui::Checkbox(define, &value))
					{
						material->setDefine(define_idx, value);
					}
				}
			}

			if (Material::getCustomFlagCount() > 0 && ImGui::CollapsingHeader("Flags"))
			{
				for (int i = 0; i < Material::getCustomFlagCount(); ++i)
				{
					bool b = material->isCustomFlag(1 << i);
					if (ImGui::Checkbox(Material::getCustomFlagName(i), &b))
					{
						if (b) material->setCustomFlag(1 << i);
						else material->unsetCustomFlag(1 << i);
					}
				}
			}
		}
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Material"; }
	ResourceType getResourceType() const override { return Material::TYPE; }


	StudioApp& m_app;
};


struct ModelPlugin LUMIX_FINAL : public AssetBrowser::IPlugin
{
	struct Task : MT::Task {
		Task(ModelPlugin& plugin) 
			: MT::Task(plugin.m_app.getWorldEditor().getAllocator())
			, plugin(plugin)
		{}
		
		int task() override {
			plugin.createTextureTileTask();
			return 0;
		}

		ModelPlugin& plugin;
	};

	explicit ModelPlugin(StudioApp& app)
		: m_app(app)
		, m_camera_entity(INVALID_ENTITY)
		, m_mesh(INVALID_ENTITY)
		, m_pipeline(nullptr)
		, m_universe(nullptr)
		, m_is_mouse_captured(false)
		, m_tile(app.getWorldEditor().getAllocator())
		, m_texture_tile_creator(*this)
	{
		app.getAssetBrowser().registerExtension("msh", Model::TYPE);
		m_texture_tile_creator.task.create("model_tile_creator");
		createPreviewUniverse();
		createTileUniverse();
	}


	~ModelPlugin()
	{
		m_texture_tile_creator.shutdown = true;
		m_texture_tile_creator.semaphore.signal();
		m_texture_tile_creator.task.destroy();
		auto& engine = m_app.getWorldEditor().getEngine();
		engine.destroyUniverse(*m_universe);
		Pipeline::destroy(m_pipeline);
		engine.destroyUniverse(*m_tile.universe);
		Pipeline::destroy(m_tile.pipeline);
	}


	void createTextureTileTask()
	{
		while (!m_texture_tile_creator.shutdown)
		{
			m_texture_tile_creator.semaphore.wait();
			if (m_texture_tile_creator.shutdown) break;
			
			StaticString<MAX_PATH_LENGTH> tile;
			{
				MT::SpinLock lock(m_texture_tile_creator.lock);
				tile = m_texture_tile_creator.tiles.back();
				m_texture_tile_creator.tiles.pop();
			}

			IAllocator& allocator = m_app.getWorldEditor().getAllocator();

			int image_width, image_height;
			u32 hash = crc32(tile);
			StaticString<MAX_PATH_LENGTH> out_path(".lumix/asset_tiles/", hash, ".dds");
			Array<u8> resized_data(allocator);
			resized_data.resize(AssetBrowser::TILE_SIZE * AssetBrowser::TILE_SIZE * 4);
			if (PathUtils::hasExtension(tile, "dds"))
			{
				FS::OsFile file;
				if (!file.open(tile, FS::Mode::OPEN_AND_READ))
				{
					copyFile("models/editor/tile_texture.dds", out_path);
					g_log_error.log("Editor") << "Failed to load " << tile;
					continue;
				}
				Array<u8> data(allocator);
				data.resize((int)file.size());
				file.read(&data[0], data.size());
				file.close();

				crn_uint32* raw_img[cCRNMaxFaces * cCRNMaxLevels];
				crn_texture_desc desc;
				bool success = crn_decompress_dds_to_images(&data[0], data.size(), raw_img, desc);
				if (!success)
				{
					copyFile("models/editor/tile_texture.dds", out_path);
					continue;
				}
				image_width = desc.m_width;
				image_height = desc.m_height;
				stbir_resize_uint8((u8*)raw_img[0],
					image_width,
					image_height,
					0,
					&resized_data[0],
					AssetBrowser::TILE_SIZE,
					AssetBrowser::TILE_SIZE,
					0,
					4);
				crn_free_all_images(raw_img, desc);
			}
			else
			{
				int image_comp;
				auto data = stbi_load(tile, &image_width, &image_height, &image_comp, 4);
				if (!data)
				{
					g_log_error.log("Editor") << "Failed to load " << tile;
					copyFile("models/editor/tile_texture.dds", out_path);
					continue;
				}
				stbir_resize_uint8(data,
					image_width,
					image_height,
					0,
					&resized_data[0],
					AssetBrowser::TILE_SIZE,
					AssetBrowser::TILE_SIZE,
					0,
					4);
				stbi_image_free(data);
			}

			if (!saveAsDDS(out_path, &resized_data[0], AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE))
			{
				g_log_error.log("Editor") << "Failed to save " << out_path;
			}
		}
	}


	void createTileUniverse()
	{
		Engine& engine = m_app.getWorldEditor().getEngine();
		m_tile.universe = &engine.createUniverse(false);
		Renderer* renderer = (Renderer*)engine.getPluginManager().getPlugin("renderer");
		m_tile.pipeline = Pipeline::create(*renderer, Path("pipelines/main.lua"), "", engine.getAllocator());
		m_tile.pipeline->load();

		Matrix mtx;
		mtx.lookAt({10, 10, 10}, Vec3::ZERO, {0, 1, 0});
		Entity light_entity = m_tile.universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		m_tile.universe->setMatrix(light_entity, mtx);
		RenderScene* render_scene = (RenderScene*)m_tile.universe->getScene(MODEL_INSTANCE_TYPE);
		m_tile.universe->createComponent(GLOBAL_LIGHT_TYPE, light_entity);
		render_scene->setGlobalLightIntensity(light_entity, 1);
		render_scene->setGlobalLightIndirectIntensity(light_entity, 1);

		m_tile.camera_entity = m_tile.universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		m_tile.universe->createComponent(CAMERA_TYPE, m_tile.camera_entity);
		render_scene->setCameraSlot(m_tile.camera_entity, "editor");

		m_tile.pipeline->setScene(render_scene);
	}


	void createPreviewUniverse()
	{
		auto& engine = m_app.getWorldEditor().getEngine();
		m_universe = &engine.createUniverse(false);
		auto* renderer = static_cast<Renderer*>(engine.getPluginManager().getPlugin("renderer"));
		m_pipeline = Pipeline::create(*renderer, Path("pipelines/main.lua"), "", engine.getAllocator());
		m_pipeline->load();

		auto mesh_entity = m_universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		auto* render_scene = static_cast<RenderScene*>(m_universe->getScene(MODEL_INSTANCE_TYPE));
		m_mesh = mesh_entity;
		m_universe->createComponent(MODEL_INSTANCE_TYPE, mesh_entity);

		auto light_entity = m_universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		m_universe->createComponent(GLOBAL_LIGHT_TYPE, light_entity);
		render_scene->setGlobalLightIntensity(light_entity, 1);

		m_camera_entity = m_universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		m_universe->createComponent(CAMERA_TYPE, m_camera_entity);
		render_scene->setCameraSlot(m_camera_entity, "editor");

		m_pipeline->setScene(render_scene);
	}


	void showPreview(Model& model)
	{
		auto* render_scene = static_cast<RenderScene*>(m_universe->getScene(MODEL_INSTANCE_TYPE));
		if (!render_scene) return;
		if (!model.isReady()) return;

		if (render_scene->getModelInstanceModel(m_mesh) != &model)
		{
			render_scene->setModelInstancePath(m_mesh, model.getPath());
			AABB aabb = model.getAABB();

			Matrix mtx;
			Vec3 center = (aabb.max + aabb.min) * 0.5f;
			Vec3 eye = center + Vec3(1, 1, 1) * (aabb.max - aabb.min).length();
			
			mtx.lookAt(eye, center, Vec3(-1, 1, -1).normalized());
			mtx.inverse();
			m_universe->setMatrix(m_camera_entity, mtx);
		}
		ImVec2 image_size(ImGui::GetContentRegionAvailWidth(), ImGui::GetContentRegionAvailWidth());

		m_pipeline->resize((int)image_size.x, (int)image_size.y);
		m_pipeline->render();

		ImGui::Image(&m_pipeline->getRenderbuffer("default", 0), image_size);
		bool mouse_down = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1);
		if (m_is_mouse_captured && !mouse_down)
		{
			m_is_mouse_captured = false;
			SDL_ShowCursor(1);
			SDL_SetRelativeMouseMode(SDL_FALSE);
			SDL_WarpMouseInWindow(nullptr, m_captured_mouse_x, m_captured_mouse_y);
		}

		if (ImGui::GetIO().MouseClicked[1] && ImGui::IsItemHovered()) ImGui::OpenPopup("PreviewPopup");

		if (ImGui::BeginPopup("PreviewPopup"))
		{
			if (ImGui::Selectable("Save preview"))
			{
				Matrix mtx = m_universe->getMatrix(m_camera_entity);
				model.getResourceManager().load(model);
				renderTile(&model, &mtx);
			}
			ImGui::EndPopup();
		}

		if (ImGui::IsItemHovered() && mouse_down)
		{
			auto delta = m_app.getMouseMove();

			if (!m_is_mouse_captured)
			{
				m_is_mouse_captured = true;
				SDL_ShowCursor(0);
				SDL_SetRelativeMouseMode(SDL_TRUE);
				SDL_GetMouseState(&m_captured_mouse_x, &m_captured_mouse_y);
			}

			if (delta.x != 0 || delta.y != 0)
			{
				const Vec2 MOUSE_SENSITIVITY(50, 50);
				Vec3 pos = m_universe->getPosition(m_camera_entity);
				Quat rot = m_universe->getRotation(m_camera_entity);

				float yaw = -Math::signum(delta.x) * (Math::pow(Math::abs((float)delta.x / MOUSE_SENSITIVITY.x), 1.2f));
				Quat yaw_rot(Vec3(0, 1, 0), yaw);
				rot = yaw_rot * rot;
				rot.normalize();

				Vec3 pitch_axis = rot.rotate(Vec3(1, 0, 0));
				float pitch =
					-Math::signum(delta.y) * (Math::pow(Math::abs((float)delta.y / MOUSE_SENSITIVITY.y), 1.2f));
				Quat pitch_rot(pitch_axis, pitch);
				rot = pitch_rot * rot;
				rot.normalize();

				Vec3 dir = rot.rotate(Vec3(0, 0, 1));
				Vec3 origin = (model.getAABB().max + model.getAABB().min) * 0.5f;

				float dist = (origin - pos).length();
				pos = origin + dir * dist;

				m_universe->setRotation(m_camera_entity, rot);
				m_universe->setPosition(m_camera_entity, pos);
			}

		}
	}


	void onGUI(Resource* resource) override
	{
		auto* model = static_cast<Model*>(resource);
		ImGui::LabelText("Bounding radius", "%f", model->getBoundingRadius());

		auto* lods = model->getLODs();
		if (lods[0].to_mesh >= 0 && !model->isFailure())
		{
			ImGui::Separator();
			ImGui::Columns(4);
			ImGui::Text("LOD"); ImGui::NextColumn();
			ImGui::Text("Distance"); ImGui::NextColumn();
			ImGui::Text("# of meshes"); ImGui::NextColumn();
			ImGui::Text("# of triangles"); ImGui::NextColumn();
			ImGui::Separator();
			int lod_count = 1;
			for (int i = 0; i < Model::MAX_LOD_COUNT && lods[i].to_mesh >= 0; ++i)
			{
				ImGui::PushID(i);
				ImGui::Text("%d", i); ImGui::NextColumn();
				if (lods[i].distance == FLT_MAX)
				{
					ImGui::Text("Infinite");
				}
				else
				{
					float dist = sqrt(lods[i].distance);
					if (ImGui::DragFloat("", &dist))
					{
						lods[i].distance = dist * dist;
					}
				}
				ImGui::NextColumn();
				ImGui::Text("%d", lods[i].to_mesh - lods[i].from_mesh + 1); ImGui::NextColumn();
				int tri_count = 0;
				for (int j = lods[i].from_mesh; j <= lods[i].to_mesh; ++j)
				{
					tri_count += model->getMesh(j).indices_count / 3;
				}

				ImGui::Text("%d", tri_count); ImGui::NextColumn();
				++lod_count;
				ImGui::PopID();
			}

			ImGui::Columns(1);
		}

		ImGui::Separator();
		for (int i = 0; i < model->getMeshCount(); ++i)
		{
			auto& mesh = model->getMesh(i);
			if (ImGui::TreeNode(&mesh, "%s", mesh.name.length() > 0 ? mesh.name.c_str() : "N/A"))
			{
				ImGui::LabelText("Triangle count", "%d", mesh.indices_count / 3);
				ImGui::LabelText("Material", "%s", mesh.material->getPath().c_str());
				ImGui::SameLine();
				if (ImGui::Button("->"))
				{
					m_app.getAssetBrowser().selectResource(mesh.material->getPath(), true);
				}
				ImGui::TreePop();
			}
		}

		ImGui::LabelText("Bone count", "%d", model->getBoneCount());
		if (model->getBoneCount() > 0 && ImGui::CollapsingHeader("Bones"))
		{
			ImGui::Columns(3);
			for (int i = 0; i < model->getBoneCount(); ++i)
			{
				ImGui::Text("%s", model->getBone(i).name.c_str());
				ImGui::NextColumn();
				Vec3 pos = model->getBone(i).transform.pos;
				ImGui::Text("%f; %f; %f", pos.x, pos.y, pos.z);
				ImGui::NextColumn();
				Quat rot = model->getBone(i).transform.rot;
				ImGui::Text("%f; %f; %f; %f", rot.x, rot.y, rot.z, rot.w);
				ImGui::NextColumn();
			}
		}

		showPreview(*model);
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Model"; }
	ResourceType getResourceType() const override { return Model::TYPE; }


	bool saveAsDDS(const char* path, const u8* image_data, int image_width, int image_height) const
	{
		ASSERT(image_data);

		crn_uint32 size;
		crn_comp_params comp_params;
		comp_params.m_file_type = cCRNFileTypeDDS;
		comp_params.m_quality_level = cCRNMaxQualityLevel;
		comp_params.m_dxt_quality = cCRNDXTQualityNormal;
		comp_params.m_dxt_compressor_type = cCRNDXTCompressorCRN;
		comp_params.m_pProgress_func = nullptr;
		comp_params.m_pProgress_func_data = nullptr;
		comp_params.m_num_helper_threads = 3;
		comp_params.m_width = image_width;
		comp_params.m_height = image_height;
		comp_params.m_format = cCRNFmtDXT5;
		comp_params.m_pImages[0][0] = (u32*)image_data;
		crn_mipmap_params mipmap_params;
		mipmap_params.m_mode = cCRNMipModeGenerateMips;

		void* data = crn_compress(comp_params, mipmap_params, size);
		if (!data) return false;

		FS::OsFile file;
		if (file.open(path, FS::Mode::CREATE_AND_WRITE))
		{
			file.write(data, size);
			file.close();
			crn_free_block(data);
			return true;
		}

		crn_free_block(data);
		return false;
	}


	void pushTileQueue(const Path& path)
	{
		ASSERT(!m_tile.queue.full());
		WorldEditor& editor = m_app.getWorldEditor();
		Engine& engine = editor.getEngine();
		ResourceManager& resource_manager = engine.getResourceManager();

		ResourceManagerBase* manager;
		if (PathUtils::hasExtension(path.c_str(), "fab"))
		{
			manager = resource_manager.get(PrefabResource::TYPE);
		}
		else
		{
			manager = resource_manager.get(Model::TYPE);
		}
		Resource* resource = manager->load(path);
		m_tile.queue.push(resource);
	}


	void popTileQueue()
	{
		m_tile.queue.pop();
		if (m_tile.paths.empty()) return;

		Path path = m_tile.paths.back();
		m_tile.paths.pop();
		pushTileQueue(path);
	}


	void update() override
	{
		if (m_tile.frame_countdown >= 0)
		{
			--m_tile.frame_countdown;
			if (m_tile.frame_countdown == -1)
			{
				StaticString<MAX_PATH_LENGTH> path(".lumix/asset_tiles/", m_tile.path_hash, ".dds");
				saveAsDDS(path, &m_tile.data[0], AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE);

				bgfx::destroy(m_tile.texture);
			}
			return;
		}

		if (m_tile.m_entity_in_fly.isValid()) return;
		if (m_tile.queue.empty()) return;

		Resource* resource = m_tile.queue.front();
		if (resource->isFailure())
		{
			g_log_error.log("Editor") << "Failed to load " << resource->getPath();
			popTileQueue();
			return;
		}
		if (!resource->isReady()) return;

		popTileQueue();

		if (resource->getType() == Model::TYPE)
		{
			renderTile((Model*)resource, nullptr);
		}
		else if (resource->getType() == PrefabResource::TYPE)
		{
			renderTile((PrefabResource*)resource);
		}
		else
		{
			ASSERT(false);
		}
	}


	void renderTile(PrefabResource* prefab)
	{
		Engine& engine = m_app.getWorldEditor().getEngine();
		RenderScene* render_scene = (RenderScene*)m_tile.universe->getScene(MODEL_INSTANCE_TYPE);
		if (!render_scene) return;

		Renderer* renderer = (Renderer*)engine.getPluginManager().getPlugin("renderer");
		if (!renderer) return;

		Entity mesh_entity = m_tile.universe->instantiatePrefab(*prefab, Vec3::ZERO, Quat::IDENTITY, 1);
		if (!mesh_entity.isValid()) return;

		if (!render_scene->getUniverse().hasComponent(mesh_entity, MODEL_INSTANCE_TYPE)) return;

		Model* model = render_scene->getModelInstanceModel(mesh_entity);
		if (!model) return;

		m_tile.path_hash = prefab->getPath().getHash();
		prefab->getResourceManager().unload(*prefab);
		m_tile.m_entity_in_fly = mesh_entity;
		model->onLoaded<ModelPlugin, &ModelPlugin::renderPrefabSecondStage>(this);
	}


	void renderPrefabSecondStage(Resource::State old_state, Resource::State new_state, Resource& resource)
	{
		Engine& engine = m_app.getWorldEditor().getEngine();
		
		RenderScene* render_scene = (RenderScene*)m_tile.universe->getScene(MODEL_INSTANCE_TYPE);
		if (!render_scene) return;

		Renderer* renderer = (Renderer*)engine.getPluginManager().getPlugin("renderer");
		if (!renderer) return;

		Model* model = (Model*)&resource;
		if (!model->isReady()) return;

		AABB aabb = model->getAABB();

		Matrix mtx;
		Vec3 center = (aabb.max + aabb.min) * 0.5f;
		Vec3 eye = center + Vec3(1, 1, 1) * (aabb.max - aabb.min).length() / Math::SQRT2;
		mtx.lookAt(eye, center, Vec3(-1, 1, -1).normalized());
		mtx.inverse();
		m_tile.universe->setMatrix(m_tile.camera_entity, mtx);

		m_tile.pipeline->resize(AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE);
		m_tile.pipeline->render();

		m_tile.texture =
			bgfx::createTexture2D(AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_READ_BACK);
		renderer->viewCounterAdd();
		bgfx::touch(renderer->getViewCounter());
		bgfx::setViewName(renderer->getViewCounter(), "billboard_blit");
		bgfx::TextureHandle color_renderbuffer = m_tile.pipeline->getRenderbuffer("default", 0);
		bgfx::blit(renderer->getViewCounter(), m_tile.texture, 0, 0, color_renderbuffer);

		renderer->viewCounterAdd();
		bgfx::setViewName(renderer->getViewCounter(), "billboard_read");
		m_tile.data.resize(AssetBrowser::TILE_SIZE * AssetBrowser::TILE_SIZE * 4);
		bgfx::readTexture(m_tile.texture, &m_tile.data[0]);
		bgfx::touch(renderer->getViewCounter());
		m_tile.universe->destroyEntity(m_tile.m_entity_in_fly);

		m_tile.frame_countdown = 2;
		m_tile.m_entity_in_fly = INVALID_ENTITY;
	}


	void renderTile(Model* model, Matrix* in_mtx)
	{
		Engine& engine = m_app.getWorldEditor().getEngine();
		RenderScene* render_scene = (RenderScene*)m_tile.universe->getScene(MODEL_INSTANCE_TYPE);
		if (!render_scene) return;

		Renderer* renderer = (Renderer*)engine.getPluginManager().getPlugin("renderer");
		if (!renderer) return;

		Entity mesh_entity = m_tile.universe->createEntity({ 0, 0, 0 }, { 0, 0, 0, 1 });
		m_tile.universe->createComponent(MODEL_INSTANCE_TYPE, mesh_entity);

		render_scene->setModelInstancePath(mesh_entity, model->getPath());
		AABB aabb = model->getAABB();

		Matrix mtx;
		Vec3 center = (aabb.max + aabb.min) * 0.5f;
		Vec3 eye = center + Vec3(1, 1, 1) * (aabb.max - aabb.min).length() / Math::SQRT2;
		mtx.lookAt(eye, center, Vec3(-1, 1, -1).normalized());
		mtx.inverse();
		if (in_mtx)
		{
			mtx = *in_mtx;
		}
		m_tile.universe->setMatrix(m_tile.camera_entity, mtx);

		m_tile.pipeline->resize(AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE);
		m_tile.pipeline->render();

		m_tile.texture =
			bgfx::createTexture2D(AssetBrowser::TILE_SIZE, AssetBrowser::TILE_SIZE, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_READ_BACK);
		renderer->viewCounterAdd();
		bgfx::touch(renderer->getViewCounter());
		bgfx::setViewName(renderer->getViewCounter(), "billboard_blit");
		bgfx::TextureHandle color_renderbuffer = m_tile.pipeline->getRenderbuffer("default", 0);
		bgfx::blit(renderer->getViewCounter(), m_tile.texture, 0, 0, color_renderbuffer);

		renderer->viewCounterAdd();
		bgfx::setViewName(renderer->getViewCounter(), "billboard_read");
		m_tile.data.resize(AssetBrowser::TILE_SIZE * AssetBrowser::TILE_SIZE * 4);
		bgfx::readTexture(m_tile.texture, &m_tile.data[0]);
		bgfx::touch(renderer->getViewCounter());
		m_tile.universe->destroyEntity(mesh_entity);

		m_tile.frame_countdown = 2;
		m_tile.path_hash = model->getPath().getHash();
		model->getResourceManager().unload(*model);
	}


	bool createTile(const char* in_path, const char* out_path, ResourceType type) override
	{
		if (type == Texture::TYPE)
		{
			MT::SpinLock lock(m_texture_tile_creator.lock);
			m_texture_tile_creator.tiles.emplace(in_path);
			m_texture_tile_creator.semaphore.signal();
			return true;
		}
		if (type == Material::TYPE) return copyFile("models/editor/tile_material.dds", out_path);
		if (type == Shader::TYPE) return copyFile("models/editor/tile_shader.dds", out_path);

		if (type != Model::TYPE && type != PrefabResource::TYPE) return false;

		Path path(in_path);

		if (!m_tile.queue.full())
		{
			pushTileQueue(path);
			return true;
		}
		
		m_tile.paths.push(path);
		return true;
	}


	struct TileData
	{
		TileData(IAllocator& allocator) 
			: data(allocator)
			, queue(allocator)
			, paths(allocator)
		{}

		Universe* universe = nullptr;
		Pipeline* pipeline = nullptr;
		Entity m_entity_in_fly = INVALID_ENTITY;
		Entity camera_entity = INVALID_ENTITY;
		int frame_countdown = -1;
		u32 path_hash;
		Array<u8> data;
		bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
		Queue<Resource*, 8> queue;
		Array<Path> paths;
	} m_tile;


	StudioApp& m_app;
	Universe* m_universe;
	Pipeline* m_pipeline;
	Entity m_mesh;
	Entity m_camera_entity;
	bool m_is_mouse_captured;
	int m_captured_mouse_x;
	int m_captured_mouse_y;
	
	struct TextureTileCreator
	{
		explicit TextureTileCreator(ModelPlugin& plugin)
			: tiles(plugin.m_app.getWorldEditor().getAllocator())
			, lock(false)
			, task(plugin)
			, semaphore(0, 64 * 1024)
		{
		}

		volatile bool shutdown = false;
		MT::SpinMutex lock;
		MT::Semaphore semaphore;
		Array<StaticString<MAX_PATH_LENGTH>> tiles;
		Task task;
	} m_texture_tile_creator;
};


struct TexturePlugin LUMIX_FINAL : public AssetBrowser::IPlugin
{
	explicit TexturePlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetBrowser().registerExtension("tga", Texture::TYPE);
		app.getAssetBrowser().registerExtension("dds", Texture::TYPE);
		app.getAssetBrowser().registerExtension("raw", Texture::TYPE);
	}


	void onGUI(Resource* resource) override
	{
		auto* texture = static_cast<Texture*>(resource);

		ImGui::LabelText("Size", "%dx%d", texture->width, texture->height);
		ImGui::LabelText("Mips", "%d", texture->mips);
		if (texture->bytes_per_pixel > 0) ImGui::LabelText("BPP", "%d", texture->bytes_per_pixel);
		if (texture->is_cubemap)
		{
			ImGui::Text("Cubemap");
			return;
		}

		if (bgfx::isValid(texture->handle))
		{
			ImVec2 texture_size(200, 200);
			if (texture->width > texture->height)
			{
				texture_size.y = texture_size.x * texture->height / texture->width;
			}
			else
			{
				texture_size.x = texture_size.y * texture->width / texture->height;
			}

			ImGui::Image(&texture->handle, texture_size);
		
			if (ImGui::Button("Open")) m_app.getAssetBrowser().openInExternalEditor(resource);
		}
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Texture"; }
	ResourceType getResourceType() const override { return Texture::TYPE; }


	StudioApp& m_app;
};


struct ShaderPlugin LUMIX_FINAL : public AssetBrowser::IPlugin
{
	explicit ShaderPlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetBrowser().registerExtension("shd", Shader::TYPE);
		app.getAssetBrowser().registerExtension("shb", ShaderBinary::TYPE);
	}


	void onGUI(Resource* resource) override
	{
		auto* shader = static_cast<Shader*>(resource);
		char basename[MAX_PATH_LENGTH];
		PathUtils::getBasename(basename, lengthOf(basename), resource->getPath().c_str());
		StaticString<MAX_PATH_LENGTH> path("/pipelines/", basename, "/", basename);
		if (ImGui::Button("Open vertex shader"))
		{
			path << "_vs.sc";
			m_app.getAssetBrowser().openInExternalEditor(path);
		}
		ImGui::SameLine();
		if (ImGui::Button("Open fragment shader"))
		{
			path << "_fs.sc";
			m_app.getAssetBrowser().openInExternalEditor(path);
		}

		if (shader->m_texture_slot_count > 0 &&
			ImGui::CollapsingHeader(
				"Texture slots", nullptr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed))
		{
			ImGui::Columns(2);
			ImGui::Text("name");
			ImGui::NextColumn();
			ImGui::Text("uniform");
			ImGui::NextColumn();
			ImGui::Separator();
			for (int i = 0; i < shader->m_texture_slot_count; ++i)
			{
				auto& slot = shader->m_texture_slots[i];
				ImGui::Text("%s", slot.name);
				ImGui::NextColumn();
				ImGui::Text("%s", slot.uniform);
				ImGui::NextColumn();
			}
			ImGui::Columns(1);
		}

		if (!shader->m_uniforms.empty() &&
			ImGui::CollapsingHeader("Uniforms", nullptr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed))
		{
			ImGui::Columns(2);
			ImGui::Text("name");
			ImGui::NextColumn();
			ImGui::Text("type");
			ImGui::NextColumn();
			ImGui::Separator();
			for (int i = 0; i < shader->m_uniforms.size(); ++i)
			{
				auto& uniform = shader->m_uniforms[i];
				ImGui::Text("%s", uniform.name);
				ImGui::NextColumn();
				switch (uniform.type)
				{
					case Shader::Uniform::COLOR: ImGui::Text("color"); break;
					case Shader::Uniform::FLOAT: ImGui::Text("float"); break;
					case Shader::Uniform::INT: ImGui::Text("int"); break;
					case Shader::Uniform::MATRIX4: ImGui::Text("Matrix 4x4"); break;
					case Shader::Uniform::TIME: ImGui::Text("time"); break;
					case Shader::Uniform::VEC4: ImGui::Text("Vector4"); break;
					case Shader::Uniform::VEC3: ImGui::Text("Vector3"); break;
					case Shader::Uniform::VEC2: ImGui::Text("Vector2"); break;
					default: ASSERT(false); break;
				}
				ImGui::NextColumn();
			}
			ImGui::Columns(1);
		}
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Shader"; }
	ResourceType getResourceType() const override { return Shader::TYPE; }


	StudioApp& m_app;
};


struct EnvironmentProbePlugin LUMIX_FINAL : public PropertyGrid::IPlugin
{
	explicit EnvironmentProbePlugin(StudioApp& app)
		: m_app(app)
	{
		WorldEditor& world_editor = app.getWorldEditor();
		PluginManager& plugin_manager = world_editor.getEngine().getPluginManager();
		Renderer*  renderer = static_cast<Renderer*>(plugin_manager.getPlugin("renderer"));
		IAllocator& allocator = world_editor.getAllocator();
		Path pipeline_path("pipelines/main.lua");
		m_pipeline = Pipeline::create(*renderer, pipeline_path, "PROBE", allocator);
		m_pipeline->load();

		m_cl_context = nullptr; //cmft::clLoad() > 0 ? cmft::clInit() : nullptr;
	}


	~EnvironmentProbePlugin()
	{
		if (m_cl_context)
		{
			cmft::clDestroy(m_cl_context);
			cmft::clUnload();
		}
		Pipeline::destroy(m_pipeline);
	}


	bool saveCubemap(ComponentUID cmp, const u8* data, int texture_size, const char* postfix)
	{
		crn_uint32 size;
		crn_comp_params comp_params;
		comp_params.m_width = texture_size;
		comp_params.m_height = texture_size;
		comp_params.m_file_type = cCRNFileTypeDDS;
		comp_params.m_format = cCRNFmtDXT1;
		comp_params.m_quality_level = cCRNMinQualityLevel;
		comp_params.m_dxt_quality = cCRNDXTQualitySuperFast;
		comp_params.m_dxt_compressor_type = cCRNDXTCompressorRYG;
		comp_params.m_pProgress_func = nullptr;
		comp_params.m_pProgress_func_data = nullptr;
		comp_params.m_num_helper_threads = MT::getCPUsCount() - 1;
		comp_params.m_faces = 6;
		for (int i = 0; i < 6; ++i)
		{
			comp_params.m_pImages[i][0] = (u32*)&data[i * texture_size * texture_size * 4];
		}
		crn_mipmap_params mipmap_params;
		mipmap_params.m_mode = cCRNMipModeGenerateMips;

		void* compressed_data = crn_compress(comp_params, mipmap_params, size);
		if (!compressed_data)
		{
			g_log_error.log("Editor") << "Failed to compress the probe.";
			return false;
		}

		FS::OsFile file;
		const char* base_path = m_app.getWorldEditor().getEngine().getDiskFileDevice()->getBasePath();
		StaticString<MAX_PATH_LENGTH> path(base_path, "universes/", m_app.getWorldEditor().getUniverse()->getName());
		if (!PlatformInterface::makePath(path) && !PlatformInterface::dirExists(path))
		{
			g_log_error.log("Editor") << "Failed to create " << path;
		}
		path << "/probes/";
		if (!PlatformInterface::makePath(path) && !PlatformInterface::dirExists(path))
		{
			g_log_error.log("Editor") << "Failed to create " << path;
		}
		u64 probe_guid = ((RenderScene*)cmp.scene)->getEnvironmentProbeGUID(cmp.entity);
		path << probe_guid << postfix << ".dds";
		if (!file.open(path, FS::Mode::CREATE_AND_WRITE))
		{
			g_log_error.log("Editor") << "Failed to create " << path;
			crn_free_block(compressed_data);
			return false;
		}

		file.write((const char*)compressed_data, size);
		file.close();
		crn_free_block(compressed_data);
		return true;
	}


	void flipY(u32* data, int texture_size)
	{
		for (int y = 0; y < texture_size / 2; ++y)
		{
			for (int x = 0; x < texture_size; ++x)
			{
				u32 t = data[x + y * texture_size];
				data[x + y * texture_size] = data[x + (texture_size - y - 1) * texture_size];
				data[x + (texture_size - y - 1) * texture_size] = t;
			}
		}
	}


	void flipX(u32* data, int texture_size)
	{
		for (int y = 0; y < texture_size; ++y)
		{
			u32* tmp = (u32*)&data[y * texture_size];
			for (int x = 0; x < texture_size / 2; ++x)
			{
				u32 t = tmp[x];
				tmp[x] = tmp[texture_size - x - 1];
				tmp[texture_size - x - 1] = t;
			}
		}
	}


	void generateCubemap(ComponentUID cmp)
	{
		static const int TEXTURE_SIZE = 1024;

		Universe* universe = m_app.getWorldEditor().getUniverse();
		if (universe->getName()[0] == '\0')
		{
			g_log_error.log("Editor") << "Universe must be saved before environment probe can be generated.";
			return;
		}

		WorldEditor& world_editor = m_app.getWorldEditor();
		Engine& engine = world_editor.getEngine();
		auto& plugin_manager = engine.getPluginManager();
		IAllocator& allocator = engine.getAllocator();

		Vec3 probe_position = universe->getPosition(cmp.entity);
		auto* scene = static_cast<RenderScene*>(universe->getScene(CAMERA_TYPE));
		Entity camera_entity = scene->getCameraInSlot("probe");
		if (!camera_entity.isValid())
		{
			g_log_error.log("Renderer") << "No camera in slot 'probe'.";
			return;
		}

		scene->setCameraFOV(camera_entity, Math::degreesToRadians(90));

		m_pipeline->setScene(scene);
		m_pipeline->resize(TEXTURE_SIZE, TEXTURE_SIZE);

		Renderer* renderer = static_cast<Renderer*>(plugin_manager.getPlugin("renderer"));

		Vec3 dirs[] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
		Vec3 ups[] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
		Vec3 ups_opengl[] = { { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } };

		Array<u8> data(allocator);
		data.resize(6 * TEXTURE_SIZE * TEXTURE_SIZE * 4);
		bgfx::TextureHandle texture =
			bgfx::createTexture2D(TEXTURE_SIZE, TEXTURE_SIZE, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_READ_BACK);
		renderer->frame(false); // submit
		renderer->frame(false); // wait for gpu

		for (int i = 0; i < 6; ++i)
		{
			Matrix mtx = Matrix::IDENTITY;
			mtx.setTranslation(probe_position);
			bool ndc_bottom_left = bgfx::getCaps()->originBottomLeft;
			Vec3 side = crossProduct(ndc_bottom_left ? ups_opengl[i] : ups[i], dirs[i]);
			mtx.setZVector(dirs[i]);
			mtx.setYVector(ndc_bottom_left ? ups_opengl[i] : ups[i]);
			mtx.setXVector(side);
			universe->setMatrix(camera_entity, mtx);
			m_pipeline->render();

			renderer->viewCounterAdd();
			bgfx::touch(renderer->getViewCounter());
			bgfx::setViewName(renderer->getViewCounter(), "probe_blit");
			bgfx::TextureHandle color_renderbuffer = m_pipeline->getRenderbuffer("default", 0);
			bgfx::blit(renderer->getViewCounter(), texture, 0, 0, color_renderbuffer);

			renderer->viewCounterAdd();
			bgfx::setViewName(renderer->getViewCounter(), "probe_read");
			bgfx::readTexture(texture, &data[i * TEXTURE_SIZE * TEXTURE_SIZE * 4]);
			bgfx::touch(renderer->getViewCounter());
			renderer->frame(false); // submit
			renderer->frame(false); // wait for gpu

			if (ndc_bottom_left) continue;

			u32* tmp = (u32*)&data[i * TEXTURE_SIZE * TEXTURE_SIZE * 4];
			if (i == 2 || i == 3)
			{
				flipY(tmp, TEXTURE_SIZE);
			}
			else
			{
				flipX(tmp, TEXTURE_SIZE);
			}
		}
		cmft::Image image;
		cmft::Image irradiance;

		cmft::imageCreate(image, TEXTURE_SIZE, TEXTURE_SIZE, 0x303030ff, 1, 6, cmft::TextureFormat::RGBA8);
		cmft::imageFromRgba32f(image, cmft::TextureFormat::RGBA8);
		copyMemory(image.m_data, &data[0], data.size());
		cmft::imageToRgba32f(image);
		
		cmft::imageRadianceFilter(
			image
			, 128
			, cmft::LightingModel::BlinnBrdf
			, false
			, 1
			, 10
			, 1
			, cmft::EdgeFixup::None
			, m_cl_context ? 0 : MT::getCPUsCount()
			, m_cl_context
		);

		cmft::imageIrradianceFilterSh(irradiance, 32, image);

		cmft::imageFromRgba32f(image, cmft::TextureFormat::RGBA8);
		cmft::imageFromRgba32f(irradiance, cmft::TextureFormat::RGBA8);



		int irradiance_size = 32;
		int radiance_size = 128;
		int reflection_size = TEXTURE_SIZE;

		if (scene->isEnvironmentProbeCustomSize(cmp.entity))
		{
			irradiance_size = scene->getEnvironmentProbeIrradianceSize(cmp.entity);
			radiance_size = scene->getEnvironmentProbeRadianceSize(cmp.entity);
			reflection_size = scene->getEnvironmentProbeReflectionSize(cmp.entity);
		}

		saveCubemap(cmp, (u8*)irradiance.m_data, irradiance_size, "_irradiance");
		saveCubemap(cmp, (u8*)image.m_data, radiance_size, "_radiance");
		if (scene->isEnvironmentProbeReflectionEnabled(cmp.entity))
		{
			saveCubemap(cmp, &data[0], reflection_size, "");
		}
		bgfx::destroy(texture);
		
		scene->reloadEnvironmentProbe(cmp.entity);
	}


	void onGUI(PropertyGrid& grid, ComponentUID cmp) override
	{
		if (cmp.type != ENVIRONMENT_PROBE_TYPE) return;

		auto* scene = static_cast<RenderScene*>(cmp.scene);
		auto* texture = scene->getEnvironmentProbeTexture(cmp.entity);
		if (texture)
		{
			ImGui::LabelText("Reflection path", "%s", texture->getPath().c_str());
			if (ImGui::Button("View reflection")) m_app.getAssetBrowser().selectResource(texture->getPath(), true);
		}
		texture = scene->getEnvironmentProbeIrradiance(cmp.entity);
		if (texture)
		{
			ImGui::LabelText("Irradiance path", "%s", texture->getPath().c_str());
			if (ImGui::Button("View irradiance")) m_app.getAssetBrowser().selectResource(texture->getPath(), true);
		}
		texture = scene->getEnvironmentProbeRadiance(cmp.entity);
		if (texture)
		{
			ImGui::LabelText("Radiance path", "%s", texture->getPath().c_str());
			if (ImGui::Button("View radiance")) m_app.getAssetBrowser().selectResource(texture->getPath(), true);
		}
		if (ImGui::Button("Generate")) generateCubemap(cmp);
	}


	StudioApp& m_app;
	Pipeline* m_pipeline;

	cmft::ClContext* m_cl_context;
};


struct EmitterPlugin LUMIX_FINAL : public PropertyGrid::IPlugin
{
	explicit EmitterPlugin(StudioApp& app)
		: m_app(app)
	{
		m_particle_emitter_updating = true;
		m_particle_emitter_timescale = 1.0f;
	}


	void onGUI(PropertyGrid& grid, ComponentUID cmp) override
	{
		if (cmp.type != PARTICLE_EMITTER_TYPE) return;
		
		ImGui::Separator();
		ImGui::Checkbox("Update", &m_particle_emitter_updating);
		auto* scene = static_cast<RenderScene*>(cmp.scene);
		ImGui::SameLine();
		if (ImGui::Button("Reset")) scene->resetParticleEmitter(cmp.entity);

		if (m_particle_emitter_updating)
		{
			ImGui::DragFloat("Timescale", &m_particle_emitter_timescale, 0.01f, 0.01f, 10000.0f);
			float time_delta = m_app.getWorldEditor().getEngine().getLastTimeDelta();
			scene->updateEmitter(cmp.entity, time_delta * m_particle_emitter_timescale);
			scene->getParticleEmitter(cmp.entity)->drawGizmo(m_app.getWorldEditor(), *scene);
		}
	}


	StudioApp& m_app;
	float m_particle_emitter_timescale;
	bool m_particle_emitter_updating;
};


struct TerrainPlugin LUMIX_FINAL : public PropertyGrid::IPlugin
{
	explicit TerrainPlugin(StudioApp& app)
		: m_app(app)
	{
		WorldEditor& editor = app.getWorldEditor();
		m_terrain_editor = LUMIX_NEW(editor.getAllocator(), TerrainEditor)(editor, app);
	}


	~TerrainPlugin()
	{
		LUMIX_DELETE(m_app.getWorldEditor().getAllocator(), m_terrain_editor);
	}


	void onGUI(PropertyGrid& grid, ComponentUID cmp) override
	{
		if (cmp.type != TERRAIN_TYPE) return;

		m_terrain_editor->setComponent(cmp);
		m_terrain_editor->onGUI();
	}


	StudioApp& m_app;
	TerrainEditor* m_terrain_editor;
};


struct FurPainter LUMIX_FINAL : public WorldEditor::Plugin
{
	explicit FurPainter(StudioApp& _app)
		: app(_app)
		, brush_radius(0.1f)
		, brush_strength(1.0f)
		, enabled(false)
	{
		app.getWorldEditor().addPlugin(*this);
	}


	void saveTexture()
	{
		WorldEditor& editor = app.getWorldEditor();
		auto& entities = editor.getSelectedEntities();
		if (entities.empty()) return;

		ComponentUID model_instance = editor.getUniverse()->getComponent(entities[0], MODEL_INSTANCE_TYPE);
		if (!model_instance.isValid()) return;

		RenderScene* scene = static_cast<RenderScene*>(model_instance.scene);
		Model* model = scene->getModelInstanceModel(model_instance.entity);

		if (!model || !model->isReady()) return;

		Texture* texture = model->getMesh(0).material->getTexture(0);
		texture->save();
	}


	struct Vertex
	{
		Vec2 uv;
		Vec3 pos;

		void fixUV(int w, int h)
		{
			if (uv.y < 0) uv.y = 1 + uv.y;
			uv.x *= (float)w;
			uv.y *= (float)h;
		}
	};


	struct Point
	{
		i64 x, y;
	};


	static i64 orient2D(const Point& a, const Point& b, const Point& c)
	{
		return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	}


	void postprocess()
	{
		WorldEditor& editor = app.getWorldEditor();
		Universe* universe = editor.getUniverse();
		auto& entities = editor.getSelectedEntities();
		if (entities.empty()) return;

		ComponentUID model_instance = universe->getComponent(entities[0], MODEL_INSTANCE_TYPE);
		if (!model_instance.isValid()) return;

		RenderScene* scene = static_cast<RenderScene*>(model_instance.scene);
		Model* model = scene->getModelInstanceModel(model_instance.entity);

		if (!model || !model->isReady() || model->getMeshCount() < 1) return;
		if (!model->getMesh(0).material) return;

		Texture* texture = model->getMesh(0).material->getTexture(0);
		if (!texture || texture->data.empty()) return;

		u8* mem = (u8*)app.getWorldEditor().getAllocator().allocate(texture->width * texture->height);

		ASSERT(!texture->data.empty());

		for (int i = 0, c = model->getMeshCount(); i < c; ++i)
		{
			Mesh& mesh = model->getMesh(i);
			const u16* idx16 = (const u16*)&mesh.indices[0];
			const u32* idx32 = (const u32*)&mesh.indices[0];
			const Vec3* vertices = &mesh.vertices[0];
			setMemory(mem, 0, texture->width * texture->height);
			for (int i = 0, c = mesh.indices_count; i < c; i += 3)
			{
				u32 idx[3];
				if (idx16)
				{
					idx[0] = idx16[i];
					idx[1] = idx16[i + 1];
					idx[2] = idx16[i + 2];
				}
				else
				{
					idx[0] = idx32[i];
					idx[1] = idx32[i + 1];
					idx[2] = idx32[i + 2];
				}

				Vertex v[] = { { mesh.uvs[idx[0]], vertices[idx[0]] },
				{ mesh.uvs[idx[1]], vertices[idx[1]] },
				{ mesh.uvs[idx[2]], vertices[idx[2]] } };

				Vec3 n = crossProduct(Vec3(v[0].uv, 0) - Vec3(v[1].uv, 0), Vec3(v[2].uv, 0) - Vec3(v[1].uv, 0));
				if (n.z > 0) Math::swap(v[1], v[2]);

				v[0].fixUV(texture->width, texture->height);
				v[1].fixUV(texture->width, texture->height);
				v[2].fixUV(texture->width, texture->height);

				rasterizeTriangle2(texture->width, mem, v);
			}
		}

		u32* data = (u32*)&texture->data[0];
		struct DistanceFieldCell
		{
			u32 distance;
			u32 color;
		};

		Array<DistanceFieldCell> distance_field(app.getWorldEditor().getAllocator());
		int width = texture->width;
		int height = texture->height;
		distance_field.resize(width * height);

		for (int j = 0; j < height; ++j)
		{
			for (int i = 0; i < width; ++i)
			{
				distance_field[i + j * width].color = data[i + j * width];
				distance_field[i + j * width].distance = 0xffffFFFF;
			}
		}

		for (int j = 1; j < height; ++j)
		{
			for (int i = 1; i < width; ++i)
			{
				int idx = i + j * width;
				if (mem[idx])
				{
					distance_field[idx].distance = 0;
				}
				else
				{
					if (distance_field[idx - 1].distance < distance_field[idx - width].distance)
					{
						distance_field[idx].distance = distance_field[idx - 1].distance + 1;
						distance_field[idx].color = distance_field[idx - 1].color;
					}
					else
					{
						distance_field[idx].distance = distance_field[idx - width].distance + 1;
						distance_field[idx].color = distance_field[idx - width].color;
					}
				}
			}
		}

		for (int j = height - 2; j >= 0; --j)
		{
			for (int i = width - 2; i >= 0; --i)
			{
				int idx = i + j * width;
				if (distance_field[idx + 1].distance < distance_field[idx + width].distance &&
					distance_field[idx + 1].distance < distance_field[idx].distance)
				{
					distance_field[idx].distance = distance_field[idx + 1].distance + 1;
					distance_field[idx].color = distance_field[idx + 1].color;
				}
				else if (distance_field[idx + width].distance < distance_field[idx].distance)
				{
					distance_field[idx].distance = distance_field[idx + width].distance + 1;
					distance_field[idx].color = distance_field[idx + width].color;
				}
			}
		}

		for (int j = 0; j < height; ++j)
		{
			for (int i = 0; i < width; ++i)
			{
				data[i + j * width] = distance_field[i + j*width].color;
			}
		}

		texture->onDataUpdated(0, 0, texture->width, texture->height);
		app.getWorldEditor().getAllocator().deallocate(mem);
	}


	void rasterizeTriangle2(int width, u8* mem, Vertex v[3]) const
	{
		static const i64 substep = 256;
		static const i64 submask = substep - 1;
		static const i64 stepshift = 8;

		Point v0 = { i64(v[0].uv.x * substep), i64(v[0].uv.y * substep) };
		Point v1 = { i64(v[1].uv.x * substep), i64(v[1].uv.y * substep) };
		Point v2 = { i64(v[2].uv.x * substep), i64(v[2].uv.y * substep) };

		i64 minX = Math::minimum(v0.x, v1.x, v2.x);
		i64 minY = Math::minimum(v0.y, v1.y, v2.y);
		i64 maxX = Math::maximum(v0.x, v1.x, v2.x) + substep;
		i64 maxY = Math::maximum(v0.y, v1.y, v2.y) + substep;

		minX = ((minX + submask) & ~submask) - 1;
		minY = ((minY + submask) & ~submask) - 1;

		Point p;
		for (p.y = minY; p.y <= maxY; p.y += substep)
		{
			for (p.x = minX; p.x <= maxX; p.x += substep)
			{
				i64 w0 = orient2D(v1, v2, p);
				i64 w1 = orient2D(v2, v0, p);
				i64 w2 = orient2D(v0, v1, p);

				if (w0 >= 0 && w1 >= 0 && w2 >= 0)
				{
					mem[(p.x >> stepshift) + (p.y >> stepshift) * width] = 1;
				}
			}
		}
	}


	void rasterizeTriangle(Texture* texture, Vertex v[3], const Vec3& center) const
	{
		float squared_radius_rcp = 1.0f / (brush_radius * brush_radius);

		static const i64 substep = 256;
		static const i64 submask = substep - 1;
		static const i64 stepshift = 8;

		Point v0 = {i64(v[0].uv.x * substep), i64(v[0].uv.y * substep)};
		Point v1 = {i64(v[1].uv.x * substep), i64(v[1].uv.y * substep)};
		Point v2 = {i64(v[2].uv.x * substep), i64(v[2].uv.y * substep)};

		i64 minX = Math::minimum(v0.x, v1.x, v2.x);
		i64 minY = Math::minimum(v0.y, v1.y, v2.y);
		i64 maxX = Math::maximum(v0.x, v1.x, v2.x) + substep;
		i64 maxY = Math::maximum(v0.y, v1.y, v2.y) + substep;

		minX = ((minX + submask) & ~submask) - 1;
		minY = ((minY + submask) & ~submask) - 1;

		Point p;
		for (p.y = minY; p.y <= maxY; p.y += substep)
		{
			for (p.x = minX; p.x <= maxX; p.x += substep)
			{
				i64 w0 = orient2D(v1, v2, p);
				i64 w1 = orient2D(v2, v0, p);
				i64 w2 = orient2D(v0, v1, p);

				if (w0 >= 0 && w1 >= 0 && w2 >= 0)
				{
					Vec3 pos =
						(float(w0) * v[0].pos + float(w1) * v[1].pos + float(w2) * v[2].pos) * (1.0f / (w0 + w1 + w2));
					float q = 1 - (center - pos).squaredLength() * squared_radius_rcp;
					if (q <= 0) continue;
						
					u32& val = ((u32*)&texture->data[0])[(p.x >> stepshift) + (p.y >> stepshift) * texture->width];
					float alpha = ((val & 0xff000000) >> 24) / 255.0f;
					alpha = brush_strength * q + alpha * (1 - q);
					val = (val & 0x00ffFFFF) | ((u32)(alpha * 255.0f) << 24);
				}
			}
		}
	}


	void paint(Texture* texture, Model* model, const Vec3& hit) const
	{
		ASSERT(!texture->data.empty());

		for (int i = 0, c = model->getMeshCount(); i < c; ++i)
		{
			const Mesh& mesh = model->getMesh(i);
			const u16* idx16 = (const u16*)&mesh.indices[0];
			const u32* idx32 = (const u32*)&mesh.indices[0];
			const Vec3* vertices = &mesh.vertices[0];
			Vec2 min((float)texture->width, (float)texture->height);
			Vec2 max(0, 0);
			int tri_count = 0;
			for (int i = 0, c = mesh.indices_count; i < c; i += 3)
			{
				u32 idx[3];
				if (idx16)
				{
					idx[0] = idx16[i];
					idx[1] = idx16[i + 1];
					idx[2] = idx16[i + 2];
				}
				else
				{
					idx[0] = idx32[i];
					idx[1] = idx32[i + 1];
					idx[2] = idx32[i + 2];
				}

				if (Math::getSphereTriangleIntersection(
					hit, brush_radius, vertices[idx[0]], vertices[idx[1]], vertices[idx[2]]))
				{
					Vertex v[] = { {mesh.uvs[idx[0]], vertices[idx[0]]},
						{mesh.uvs[idx[1]], vertices[idx[1]]},
						{mesh.uvs[idx[2]], vertices[idx[2]]} };

					Vec3 n = crossProduct(Vec3(v[0].uv, 0) - Vec3(v[1].uv, 0), Vec3(v[2].uv, 0) - Vec3(v[1].uv, 0));
					if (n.z > 0) Math::swap(v[1], v[2]);

					v[0].fixUV(texture->width, texture->height);
					v[1].fixUV(texture->width, texture->height);
					v[2].fixUV(texture->width, texture->height);

					min.x = Math::minimum(min.x, v[0].uv.x, v[1].uv.x, v[2].uv.x);
					max.x = Math::maximum(max.x, v[0].uv.x, v[1].uv.x, v[2].uv.x);

					min.y = Math::minimum(min.y, v[0].uv.y, v[1].uv.y, v[2].uv.y);
					max.y = Math::maximum(max.y, v[0].uv.y, v[1].uv.y, v[2].uv.y);

					++tri_count;
					rasterizeTriangle(texture, v, hit);
				}
			}

			if (tri_count > 0) texture->onDataUpdated((int)min.x, (int)min.y, int(max.x - min.x), int(max.y - min.y));
		}
	}


	bool onMouseDown(const WorldEditor::RayHit& hit, int x, int y) override
	{
		if (!hit.entity.isValid()) return false;
		auto& ents = app.getWorldEditor().getSelectedEntities();
		
		if (enabled && ents.size() == 1 && ents[0] == hit.entity)
		{
			onMouseMove(x, y, 0, 0);
			return true;
		}
		return false;
	}


	void onMouseMove(int x, int y, int, int) override
	{
		WorldEditor& editor = app.getWorldEditor();
		Universe* universe = editor.getUniverse();
		auto& entities = editor.getSelectedEntities();
		if (entities.empty()) return;
		if (!editor.isMouseDown(MouseButton::LEFT)) return;

		ComponentUID model_instance = universe->getComponent(entities[0], MODEL_INSTANCE_TYPE);
		if (!model_instance.isValid()) return;

		RenderScene* scene = static_cast<RenderScene*>(model_instance.scene);
		Model* model = scene->getModelInstanceModel(model_instance.entity);

		if (!model || !model->isReady() || model->getMeshCount() < 1) return;
		if (!model->getMesh(0).material) return;

		Texture* texture = model->getMesh(0).material->getTexture(0);
		if (!texture || texture->data.empty()) return;

		const Pose* pose = scene->lockPose(model_instance.entity);
		if (!pose) return;

		Vec3 origin, dir;
		scene->getRay(editor.getEditCamera().entity, {(float)x, (float)y}, origin, dir);
		RayCastModelHit hit = model->castRay(origin, dir, universe->getMatrix(entities[0]), pose);
		if (!hit.m_is_hit)
		{
			scene->unlockPose(model_instance.entity, false);
			return;
		}

		Vec3 hit_pos = hit.m_origin + hit.m_t * hit.m_dir;
		hit_pos = universe->getTransform(entities[0]).inverted().transform(hit_pos);

		paint(texture, model, hit_pos);
		scene->unlockPose(model_instance.entity, false);
	}


	float brush_radius;
	float brush_strength;
	StudioApp& app;
	bool enabled;
};


struct FurPainterPlugin LUMIX_FINAL : public StudioApp::GUIPlugin
{
	explicit FurPainterPlugin(StudioApp& _app)
		: app(_app)
		, is_open(false)
	{
		fur_painter = LUMIX_NEW(app.getWorldEditor().getAllocator(), FurPainter)(_app);
		Action* action = LUMIX_NEW(app.getWorldEditor().getAllocator(), Action)("Fur Painter", "Toggle fur painter", "fur_painter");
		action->func.bind<FurPainterPlugin, &FurPainterPlugin::onAction>(this);
		action->is_selected.bind<FurPainterPlugin, &FurPainterPlugin::isOpen>(this);
		app.addWindowAction(action);
	}


	~FurPainterPlugin()
	{
		app.getWorldEditor().removePlugin(*fur_painter);
		LUMIX_DELETE(app.getWorldEditor().getAllocator(), fur_painter);
	}


	const char* getName() const override { return "fur_painter"; }


	bool isOpen() const { return is_open; }
	void onAction() { is_open = !is_open; }


	void onWindowGUI() override
	{
		if (ImGui::BeginDock("Fur painter", &is_open))
		{
			ImGui::Checkbox("Enabled", &fur_painter->enabled);
			if (!fur_painter->enabled) goto end;


			WorldEditor& editor = app.getWorldEditor();
			const auto& entities = editor.getSelectedEntities();
			if (entities.empty())
			{
				ImGui::Text("No entity selected.");
				goto end;
			}
			Universe* universe = editor.getUniverse();
			RenderScene* scene = static_cast<RenderScene*>(universe->getScene(MODEL_INSTANCE_TYPE));
			ComponentUID model_instance = universe->getComponent(entities[0], MODEL_INSTANCE_TYPE);

			if (!model_instance.isValid())
			{
				ImGui::Text("Entity does not have model_instance component.");
				goto end;
			}

			Model* model = scene->getModelInstanceModel(model_instance.entity);
			if (!model)
			{
				ImGui::Text("Entity does not have model.");
				goto end;
			}

			if (model->isFailure())
			{
				ImGui::Text("Model failed to load.");
				goto end;
			}
			else if (model->isEmpty())
			{
				ImGui::Text("Model is not loaded.");
				goto end;
			}

			if(model->getMeshCount() < 1 || !model->getMesh(0).material)
			{
				ImGui::Text("Model file is invalid.");
				goto end;
			}

			Texture* texture = model->getMesh(0).material->getTexture(0);
			if (!texture)
			{
				ImGui::Text("Missing texture.");
				goto end;
			}

			if(!endsWith(texture->getPath().c_str(), ".tga"))
			{
				ImGui::Text("Only TGA can be painted");
				goto end;
			}

			if (texture->data.empty())
			{
				texture->addDataReference();
				texture->getResourceManager().reload(*texture);
				goto end;
			}

			ImGui::DragFloat("Brush radius", &fur_painter->brush_radius);
			ImGui::DragFloat("Brush strength", &fur_painter->brush_strength, 0.01f, 0.0f, 1.0f);
			if (ImGui::Button("Save texture")) fur_painter->saveTexture();
			ImGui::SameLine();
			if (ImGui::Button("Postprocess")) fur_painter->postprocess();

			drawGizmo();
		}
		
		end:
			ImGui::EndDock();
	}


	void drawGizmo()
	{
		if (!fur_painter->enabled) return;

		WorldEditor& editor = app.getWorldEditor();
		auto& entities = editor.getSelectedEntities();
		if (entities.empty()) return;

		ComponentUID model_instance = editor.getUniverse()->getComponent(entities[0], MODEL_INSTANCE_TYPE);
		if (!model_instance.isValid()) return;

		RenderScene* scene = static_cast<RenderScene*>(model_instance.scene);
		Model* model = scene->getModelInstanceModel(model_instance.entity);

		if (!model || !model->isReady() || model->getMeshCount() < 1) return;
		if (!model->getMesh(0).material) return;

		Texture* texture = model->getMesh(0).material->getTexture(0);
		if (!texture || texture->data.empty()) return;

		const Pose* pose = scene->lockPose(model_instance.entity);
		if (!pose) return;

		Vec3 origin, dir;
		scene->getRay(editor.getEditCamera().entity, editor.getMousePos(), origin, dir);
		RayCastModelHit hit = model->castRay(origin, dir, editor.getUniverse()->getMatrix(entities[0]), pose);
		if (!hit.m_is_hit)
		{
			scene->unlockPose(model_instance.entity, false);
			return;
		}

		Vec3 hit_pos = hit.m_origin + hit.m_t * hit.m_dir;
		scene->addDebugSphere(hit_pos, fur_painter->brush_radius, 0xffffFFFF, 0);
		scene->unlockPose(model_instance.entity, false);
	}


	FurPainter* fur_painter;
	bool is_open;
	StudioApp& app;
};


struct RenderInterfaceImpl LUMIX_FINAL : public RenderInterface
{
	RenderInterfaceImpl(WorldEditor& editor, Pipeline& pipeline)
		: m_pipeline(pipeline)
		, m_editor(editor)
		, m_models(editor.getAllocator())
		, m_textures(editor.getAllocator())
	{
		m_model_index = -1;
		auto& rm = m_editor.getEngine().getResourceManager();
		Path shader_path("pipelines/common/debugline.shd");
		m_shader = static_cast<Shader*>(rm.get(Shader::TYPE)->load(shader_path));

		editor.universeCreated().bind<RenderInterfaceImpl, &RenderInterfaceImpl::onUniverseCreated>(this);
		editor.universeDestroyed().bind<RenderInterfaceImpl, &RenderInterfaceImpl::onUniverseDestroyed>(this);
	}


	~RenderInterfaceImpl()
	{
		auto& rm = m_editor.getEngine().getResourceManager();
		rm.get(Shader::TYPE)->unload(*m_shader);

		m_editor.universeCreated().unbind<RenderInterfaceImpl, &RenderInterfaceImpl::onUniverseCreated>(this);
		m_editor.universeDestroyed().unbind<RenderInterfaceImpl, &RenderInterfaceImpl::onUniverseDestroyed>(this);
	}


	void addText2D(float x, float y, float font_size, u32 color, const char* text) override
	{
		auto& renderer = static_cast<Renderer&>(m_render_scene->getPlugin());
		Font* font = renderer.getFontManager().getDefaultFont();
		m_pipeline.getDraw2D().AddText(font, font_size, { x, y }, color, text);
	}


	void addRect2D(const Vec2& a, const Vec2& b, u32 color) override
	{
		m_pipeline.getDraw2D().AddRect(a, b, color);
	}


	void addRectFilled2D(const Vec2& a, const Vec2& b, u32 color) override
	{
		m_pipeline.getDraw2D().AddRectFilled(a, b, color);
	}


	Vec3 getClosestVertex(Universe* universe, Entity entity, const Vec3& wpos) override
	{
		Matrix mtx = universe->getMatrix(entity);
		Matrix inv_mtx = mtx;
		inv_mtx.inverse();
		Vec3 lpos = inv_mtx.transformPoint(wpos);
		auto* scene = (RenderScene*)universe->getScene(MODEL_INSTANCE_TYPE);
		if (!universe->hasComponent(entity, MODEL_INSTANCE_TYPE)) return wpos;

		Model* model = scene->getModelInstanceModel(entity);
		
		float min_dist_squared = FLT_MAX;
		Vec3 closest_vertex = lpos;
		auto processVertex = [&](const Vec3& vertex) {
			float dist_squared = (vertex - lpos).squaredLength();
			if (dist_squared < min_dist_squared)
			{
				min_dist_squared = dist_squared;
				closest_vertex = vertex;
			}
		};

		for (int i = 0, c = model->getMeshCount(); i < c; ++i)
		{
			Mesh& mesh = model->getMesh(i);

			if (mesh.areIndices16())
			{
				const u16* indices = (const u16*)&mesh.indices[0];
				for (int i = 0, c = mesh.indices_count; i < c; ++i)
				{
					Vec3 vertex = mesh.vertices[indices[i]];
					processVertex(vertex);
				}
			}
			else
			{
				const u32* indices = (const u32*)&mesh.indices[0];
				for (int i = 0, c = mesh.indices_count; i < c; ++i)
				{
					Vec3 vertex = mesh.vertices[indices[i]];
					processVertex(vertex);
				}
			}
		}
		return mtx.transformPoint(closest_vertex);
	}


	ImFont* addFont(const char* filename, int size) override
	{
		ImGuiIO& io = ImGui::GetIO();
		ImFont* font = io.Fonts->AddFontFromFileTTF(filename, (float)size);

		Engine& engine = m_editor.getEngine();
		unsigned char* pixels;
		int width, height;
		ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		auto* material_manager = engine.getResourceManager().get(Material::TYPE);
		Resource* resource = material_manager->load(Path("pipelines/imgui/imgui.mat"));
		Material* material = (Material*)resource;

		Texture* old_texture = material->getTexture(0);
		Texture* texture = LUMIX_NEW(engine.getAllocator(), Texture)(
			Path("font"), *engine.getResourceManager().get(Texture::TYPE), engine.getAllocator());

		texture->create(width, height, pixels);
		material->setTexture(0, texture);
		if (old_texture)
		{
			old_texture->destroy();
			LUMIX_DELETE(engine.getAllocator(), old_texture);
		}

		return font;
	}


	ModelHandle loadModel(Path& path) override
	{
		auto& rm = m_editor.getEngine().getResourceManager();
		m_models.insert(m_model_index, static_cast<Model*>(rm.get(Model::TYPE)->load(path)));
		++m_model_index;
		return m_model_index - 1;
	}


	bool saveTexture(Engine& engine, const char* path_cstr, const void* pixels, int w, int h) override
	{
		FS::FileSystem& fs = engine.getFileSystem();
		Path path(path_cstr);
		FS::IFile* file = fs.open(fs.getDefaultDevice(), path, FS::Mode::CREATE_AND_WRITE);
		if (!file) return false;

		if (!Texture::saveTGA(file, w, h, 4, (const u8*)pixels, path, engine.getAllocator()))
		{
			fs.close(*file);
			return false;
		}

		fs.close(*file);
		return true;
	}


	ImTextureID createTexture(const char* name, const void* pixels, int w, int h) override
	{
		auto& rm = m_editor.getEngine().getResourceManager();
		auto& allocator = m_editor.getAllocator();
		Texture* texture = LUMIX_NEW(allocator, Texture)(Path(name), *rm.get(Texture::TYPE), allocator);
		texture->create(w, h, pixels);
		m_textures.insert(&texture->handle, texture);
		return &texture->handle;
	}


	void destroyTexture(ImTextureID handle) override
	{
		auto& allocator = m_editor.getAllocator();
		auto iter = m_textures.find(handle);
		if (iter == m_textures.end()) return;
		auto* texture = iter.value();
		m_textures.erase(iter);
		texture->destroy();
		LUMIX_DELETE(allocator, texture);
	}


	ImTextureID loadTexture(const Path& path) override
	{
		auto& rm = m_editor.getEngine().getResourceManager();
		auto* texture = static_cast<Texture*>(rm.get(Texture::TYPE)->load(path));
		m_textures.insert(&texture->handle, texture);
		return &texture->handle;
	}


	void unloadTexture(ImTextureID handle) override
	{
		auto iter = m_textures.find(handle);
		if (iter == m_textures.end()) return;
		auto* texture = iter.value();
		texture->getResourceManager().unload(*texture);
		m_textures.erase(iter);
	}


	void addDebugCross(const Vec3& pos, float size, u32 color, float life) override
	{
		m_render_scene->addDebugCross(pos, size, color, life);
	}


	WorldEditor::RayHit castRay(const Vec3& origin, const Vec3& dir, Entity ignored) override
	{
		auto hit = m_render_scene->castRay(origin, dir, ignored);

		return{ hit.m_is_hit, hit.m_t, hit.m_entity, hit.m_origin + hit.m_dir * hit.m_t };
	}


	void getRay(Entity camera, const Vec2& screen_pos, Vec3& origin, Vec3& dir) override
	{
		m_render_scene->getRay(camera, screen_pos, origin, dir);
	}


	void addDebugLine(const Vec3& from, const Vec3& to, u32 color, float life) override
	{
		m_render_scene->addDebugLine(from, to, color, life);
	}


	void addDebugCube(const Vec3& minimum, const Vec3& maximum, u32 color, float life) override
	{
		m_render_scene->addDebugCube(minimum, maximum, color, life);
	}


	AABB getEntityAABB(Universe& universe, Entity entity) override
	{
		AABB aabb;
		
		if (universe.hasComponent(entity, MODEL_INSTANCE_TYPE))
		{
			Model* model = m_render_scene->getModelInstanceModel(entity);
			if (!model) return aabb;

			aabb = model->getAABB();
			aabb.transform(universe.getMatrix(entity));

			return aabb;
		}

		Vec3 pos = universe.getPosition(entity);
		aabb.set(pos, pos);

		return aabb;
	}


	void unloadModel(ModelHandle handle) override
	{
		auto* model = m_models[handle];
		model->getResourceManager().unload(*model);
		m_models.erase(handle);
	}


	void setCameraSlot(Entity entity, const char* slot) override
	{
		m_render_scene->setCameraSlot(entity, slot);
	}


	Entity getCameraInSlot(const char* slot) override
	{
		return m_render_scene->getCameraInSlot(slot);
	}


	Vec2 getCameraScreenSize(Entity entity) override
	{
		return m_render_scene->getCameraScreenSize(entity);
	}


	float getCameraOrthoSize(Entity entity) override
	{
		return m_render_scene->getCameraOrthoSize(entity);
	}


	bool isCameraOrtho(Entity entity) override
	{
		return m_render_scene->isCameraOrtho(entity);
	}


	float getCameraFOV(Entity entity) override
	{
		return m_render_scene->getCameraFOV(entity);
	}


	float castRay(ModelHandle model, const Vec3& origin, const Vec3& dir, const Matrix& mtx, const Pose* pose) override
	{
		RayCastModelHit hit = m_models[model]->castRay(origin, dir, mtx, pose);
		return hit.m_is_hit ? hit.m_t : -1;
	}


	void renderModel(ModelHandle model, const Matrix& mtx) override
	{
		if (!m_pipeline.isReady() || !m_models[model]->isReady()) return;

		m_pipeline.renderModel(*m_models[model], nullptr, mtx);
	}


	void onUniverseCreated()
	{
		m_render_scene = static_cast<RenderScene*>(m_editor.getUniverse()->getScene(MODEL_INSTANCE_TYPE));
	}


	void onUniverseDestroyed()
	{
		m_render_scene = nullptr;
	}


	Vec3 getModelCenter(Entity entity) override
	{
		if (!m_render_scene->getUniverse().hasComponent(entity, MODEL_INSTANCE_TYPE)) return Vec3::ZERO;
		Model* model = m_render_scene->getModelInstanceModel(entity);
		if (!model) return Vec3(0, 0, 0);
		return (model->getAABB().min + model->getAABB().max) * 0.5f;
	}


	Path getModelInstancePath(Entity entity) override
	{
		return m_render_scene->getModelInstancePath(entity);
	}


	void render(const Matrix& mtx,
		u16* indices,
		int indices_count,
		Vertex* vertices,
		int vertices_count,
		bool lines) override
	{
		if (!m_shader->isReady()) return;

		auto& renderer = static_cast<Renderer&>(m_render_scene->getPlugin());
		if (bgfx::getAvailTransientIndexBuffer(indices_count) < (u32)indices_count) return;
		if (bgfx::getAvailTransientVertexBuffer(vertices_count, renderer.getBasicVertexDecl()) < (u32)vertices_count) return;
		bgfx::TransientVertexBuffer vertex_buffer;
		bgfx::TransientIndexBuffer index_buffer;
		bgfx::allocTransientVertexBuffer(&vertex_buffer, vertices_count, renderer.getBasicVertexDecl());
		bgfx::allocTransientIndexBuffer(&index_buffer, indices_count);

		copyMemory(vertex_buffer.data, vertices, vertices_count * renderer.getBasicVertexDecl().getStride());
		copyMemory(index_buffer.data, indices, indices_count * sizeof(u16));

		u64 flags = m_shader->m_render_states;
		if (lines) flags |= BGFX_STATE_PT_LINES;
		m_pipeline.render(vertex_buffer,
			index_buffer,
			mtx,
			0,
			indices_count,
			flags,
			m_shader->getInstance(0));
	}


	Vec2 worldToScreenPixels(const Vec3& world) override
	{
		Entity camera = m_pipeline.getAppliedCamera();
		Matrix mtx = m_render_scene->getCameraViewProjection(camera);
		Vec4 pos = mtx * Vec4(world, 1);
		float inv = 1 / pos.w;
		Vec2 screen_size = m_render_scene->getCameraScreenSize(camera);
		Vec2 screen_pos = { 0.5f * pos.x * inv + 0.5f, 1.0f - (0.5f * pos.y * inv + 0.5f) };
		return screen_pos * screen_size;
	}


	Frustum getFrustum(Entity camera, const Vec2& viewport_min, const Vec2& viewport_max) override
	{
		return m_render_scene->getCameraFrustum(camera, viewport_min, viewport_max);
	}


	void getModelInstaces(Array<Entity>& entities, const Frustum& frustum, const Vec3& lod_ref_point, Entity camera) override
	{
		Array<Array<MeshInstance>>& res = m_render_scene->getModelInstanceInfos(frustum, lod_ref_point, camera, ~0ULL);
		for (auto& sub : res)
		{
			for (MeshInstance m : sub)
			{
				if (entities.indexOf(m.owner) < 0)
				{
					entities.push(m.owner);
				}
			}
		}
	}


	WorldEditor& m_editor;
	Shader* m_shader;
	RenderScene* m_render_scene;
	Pipeline& m_pipeline;
	HashMap<int, Model*> m_models;
	HashMap<void*, Texture*> m_textures;
	int m_model_index;
};



struct RenderStatsPlugin LUMIX_FINAL : public StudioApp::GUIPlugin
{
	explicit RenderStatsPlugin(StudioApp& app)
	{
		Action* action = LUMIX_NEW(app.getWorldEditor().getAllocator(), Action)("Render Stats", "Toggle render stats", "render_stats");
		action->func.bind<RenderStatsPlugin, &RenderStatsPlugin::onAction>(this);
		action->is_selected.bind<RenderStatsPlugin, &RenderStatsPlugin::isOpen>(this);
		app.addWindowAction(action);
	}


	const char* getName() const override
	{
		return "render_stats";
	}


	void onWindowGUI() override
	{
		double total_cpu = 0;
		double total_gpu = 0;
		if (ImGui::BeginDock("Renderer Stats", &m_is_open))
		{
			ImGui::Columns(3);
			ImGui::Text("%s", "View name");
			ImGui::NextColumn();
			ImGui::Text("%s", "GPU time (ms)");
			ImGui::NextColumn();
			ImGui::Text("%s", "CPU time (ms)");
			ImGui::NextColumn();
			ImGui::Separator();
			const bgfx::Stats* stats = bgfx::getStats();
			for (int i = 0; i < stats->numViews; ++i)
			{
				auto& view_stat = stats->viewStats[i];
				ImGui::Text("%s", view_stat.name);
				ImGui::NextColumn();
				double gpu_time = 1000.0f * view_stat.gpuTimeElapsed / (double)stats->gpuTimerFreq;
				ImGui::Text("%f", gpu_time);
				ImGui::NextColumn();
				double cpu_time = 1000.0f * view_stat.cpuTimeElapsed / (double)stats->cpuTimerFreq;
				ImGui::Text("%f", cpu_time);
				ImGui::NextColumn();
				total_cpu += cpu_time;
				total_gpu += gpu_time;
			}
			ImGui::Separator();
			ImGui::Text("%s", "Total");
			ImGui::NextColumn();
			ImGui::Text("%f", total_gpu);
			ImGui::NextColumn();
			ImGui::Text("%f", total_cpu);
			ImGui::NextColumn();
			ImGui::Columns();
		}
		ImGui::EndDock();
	}

	
	bool isOpen() const { return m_is_open; }
	void onAction() { m_is_open = !m_is_open; }


	bool m_is_open = false;
};


struct EditorUIRenderPlugin LUMIX_FINAL : public StudioApp::GUIPlugin
{
	EditorUIRenderPlugin(StudioApp& app, SceneView& scene_view, GameView& game_view)
		: m_app(app)
		, m_scene_view(scene_view)
		, m_game_view(game_view)
		, m_width(-1)
		, m_height(-1)
		, m_engine(app.getWorldEditor().getEngine())
	{
		WorldEditor& editor = app.getWorldEditor();

		PluginManager& plugin_manager = m_engine.getPluginManager();
		Renderer* renderer = (Renderer*)plugin_manager.getPlugin("renderer");

		int w, h;
		SDL_GetWindowSize(m_app.getWindow(), &w, &h);
		renderer->resize(w, h);

		unsigned char* pixels;
		int width, height;
		ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		auto* material_manager = m_engine.getResourceManager().get(Material::TYPE);
		Resource* resource = material_manager->load(Path("pipelines/imgui/imgui.mat"));
		m_material = static_cast<Material*>(resource);

		Texture* old_texture = m_material->getTexture(0);
		Texture* texture = LUMIX_NEW(editor.getAllocator(), Texture)(
			Path("font"), *m_engine.getResourceManager().get(Texture::TYPE), editor.getAllocator());

		texture->create(width, height, pixels);
		m_material->setTexture(0, texture);
		if (old_texture)
		{
			old_texture->destroy();
			LUMIX_DELETE(m_engine.getAllocator(), old_texture);
		}

		IAllocator& allocator = editor.getAllocator();
		RenderInterface* render_interface = LUMIX_NEW(allocator, RenderInterfaceImpl)(editor, *scene_view.getPipeline());
		editor.setRenderInterface(render_interface);

		m_index_buffer = bgfx::createDynamicIndexBuffer(1024 * 256);
		m_vertex_buffer = bgfx::createDynamicVertexBuffer(1024 * 256, renderer->getBasic2DVertexDecl());
	}


	~EditorUIRenderPlugin()
	{
		bgfx::destroy(m_index_buffer);
		bgfx::destroy(m_vertex_buffer);
		WorldEditor& editor = m_app.getWorldEditor();
		shutdownImGui();
	}


	void onWindowGUI() override { }


	const char* getName() const override { return "editor_ui_render"; }


	void shutdownImGui()
	{
		ImGui::ShutdownDock();
		ImGui::DestroyContext();

		Texture* texture = m_material->getTexture(0);
		m_material->setTexture(0, nullptr);
		texture->destroy();
		LUMIX_DELETE(m_app.getWorldEditor().getAllocator(), texture);

		m_material->getResourceManager().unload(*m_material);
	}


	u8 beginViewportRender(FrameBuffer* framebuffer)
	{
		PluginManager& plugin_manager = m_engine.getPluginManager();
		Renderer* renderer = (Renderer*)plugin_manager.getPlugin("renderer");

		renderer->viewCounterAdd();
		u8 view = (u8)renderer->getViewCounter();
		if (framebuffer)
		{
			bgfx::setViewFrameBuffer(view, framebuffer->getHandle());
		}
		else
		{
			bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
		}
		bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
		bgfx::setViewName(view, "imgui viewport");
		bgfx::setViewMode(view, bgfx::ViewMode::Sequential);

		float left = 0;
		float top = 0;
		float width = ImGui::GetIO().DisplaySize.x;
		float right = width + left;
		float height = ImGui::GetIO().DisplaySize.y;
		float bottom = height + top;
		Matrix ortho;
		ortho.setOrtho(left, right, bottom, top, -1.0f, 1.0f, bgfx::getCaps()->homogeneousDepth, true);
		if (framebuffer && (framebuffer->getWidth() != int(width + 0.5f) || framebuffer->getHeight() != int(height + 0.5f)))
		{
			framebuffer->resize((int)width, (int)height);
		}

		bgfx::setViewRect(view, 0, 0, (uint16_t)width, (uint16_t)height);
		bgfx::setViewTransform(view, nullptr, &ortho.m11);
		bgfx::touch(view);

		return view;
	}


	void guiEndFrame() override
	{
		u8 view;
		ImDrawData* draw_data = ImGui::GetDrawData();

		if (!m_material || !m_material->isReady()) goto end;
		if (!m_material->getTexture(0)) goto end;

		m_vb_offset = 0;
		m_ib_offset = 0;

		int w, h;
		SDL_GetWindowSize(m_app.getWindow(), &w, &h);
		if (w != m_width || h != m_height)
		{
			m_width = w;
			m_height = h;
			auto& plugin_manager = m_app.getWorldEditor().getEngine().getPluginManager();
			auto* renderer = static_cast<Renderer*>(plugin_manager.getPlugin("renderer"));
			if (renderer) renderer->resize(m_width, m_height);
		}

		view = beginViewportRender(nullptr);
		
		for (int i = 0; i < draw_data->CmdListsCount; ++i)
		{
			ImDrawList* cmd_list = draw_data->CmdLists[i];
			drawGUICmdList(view, cmd_list);
		}

		end:
			Renderer* renderer = static_cast<Renderer*>(m_engine.getPluginManager().getPlugin("renderer"));
			renderer->frame(false);
	}


	void drawGUICmdList(u8 view, ImDrawList* cmd_list)
	{
		Renderer* renderer = static_cast<Renderer*>(m_engine.getPluginManager().getPlugin("renderer"));
		int pass_idx = renderer->getPassIdx("MAIN");

		int num_indices = cmd_list->IdxBuffer.size();
		int num_vertices = cmd_list->VtxBuffer.size();
		auto& decl = renderer->getBasic2DVertexDecl();

		const bgfx::Memory* mem_ib = bgfx::copy(&cmd_list->IdxBuffer[0], num_indices * sizeof(u16));
		const bgfx::Memory* mem_vb = bgfx::copy(&cmd_list->VtxBuffer[0], num_vertices * decl.getStride());
		bgfx::updateDynamicIndexBuffer(m_index_buffer, m_ib_offset, mem_ib);
		bgfx::updateDynamicVertexBuffer(m_vertex_buffer, m_vb_offset, mem_vb);
		u32 elem_offset = 0;
		const ImDrawCmd* pcmd_begin = cmd_list->CmdBuffer.begin();
		const ImDrawCmd* pcmd_end = cmd_list->CmdBuffer.end();
		for (const ImDrawCmd* pcmd = pcmd_begin; pcmd != pcmd_end; pcmd++)
		{
			if (pcmd->UserCallback)
			{
				pcmd->UserCallback(cmd_list, pcmd);
				elem_offset += pcmd->ElemCount;
				continue;
			}

			if (0 == pcmd->ElemCount) continue;

			bgfx::setScissor(
				u16(Math::maximum(pcmd->ClipRect.x, 0.0f)),
				u16(Math::maximum(pcmd->ClipRect.y, 0.0f)),
				u16(Math::minimum(pcmd->ClipRect.z, 65535.0f) - Math::maximum(pcmd->ClipRect.x, 0.0f)),
				u16(Math::minimum(pcmd->ClipRect.w, 65535.0f) - Math::maximum(pcmd->ClipRect.y, 0.0f)));

			auto material = m_material;
			const auto& texture_id =
				pcmd->TextureId ? *(bgfx::TextureHandle*)pcmd->TextureId : material->getTexture(0)->handle;
			auto texture_uniform = material->getShader()->m_texture_slots[0].uniform_handle;
			u64 render_states = material->getRenderStates();
			if (&m_scene_view.getTextureHandle() == &texture_id || &m_game_view.getTextureHandle() == &texture_id)
			{
				render_states &= ~BGFX_STATE_BLEND_MASK;
			}
			bgfx::setTexture(0, texture_uniform, texture_id);
			
			ShaderInstance& shader_instance = material->getShaderInstance();
			bgfx::setStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | render_states);
			bgfx::setVertexBuffer(0, m_vertex_buffer, m_vb_offset, num_vertices);
			u32 first_index = elem_offset + m_ib_offset;
			bgfx::setIndexBuffer(m_index_buffer, first_index, pcmd->ElemCount);
			bgfx::submit(view, shader_instance.getProgramHandle(pass_idx));
			

			elem_offset += pcmd->ElemCount;
		}
		m_ib_offset += num_indices;
		m_vb_offset += num_vertices;
	}


	int m_width;
	int m_height;
	StudioApp& m_app;
	Engine& m_engine;
	Material* m_material;
	SceneView& m_scene_view;
	GameView& m_game_view;
	bgfx::DynamicVertexBufferHandle m_vertex_buffer;
	bgfx::DynamicIndexBufferHandle m_index_buffer;
	int m_vb_offset;
	int m_ib_offset;
};


struct ShaderEditorPlugin LUMIX_FINAL : public StudioApp::GUIPlugin
{
	explicit ShaderEditorPlugin(StudioApp& app)
		: m_shader_editor(app.getWorldEditor().getAllocator())
		, m_app(app)
	{
		Action* action = LUMIX_NEW(app.getWorldEditor().getAllocator(), Action)("Shader Editor", "Toggle shader editor", "shaderEditor");
		action->func.bind<ShaderEditorPlugin, &ShaderEditorPlugin::onAction>(this);
		action->is_selected.bind<ShaderEditorPlugin, &ShaderEditorPlugin::isOpen>(this);
		app.addWindowAction(action);
		m_shader_editor.m_is_open = false;

		m_compiler = LUMIX_NEW(app.getWorldEditor().getAllocator(), ShaderCompiler)(app, app.getLogUI());

		lua_State* L = app.getWorldEditor().getEngine().getState();
		auto* f =
			&LuaWrapper::wrapMethodClosure<ShaderCompiler, decltype(&ShaderCompiler::makeUpToDate), &ShaderCompiler::makeUpToDate>;
		LuaWrapper::createSystemClosure(L, "Editor", m_compiler, "compileShaders", f);
	}


	~ShaderEditorPlugin() { LUMIX_DELETE(m_app.getWorldEditor().getAllocator(), m_compiler); }


	const char* getName() const override { return "shader_editor"; }
	void update(float) override { m_compiler->update(); }
	void onAction() { m_shader_editor.m_is_open = !m_shader_editor.m_is_open; }
	void onWindowGUI() override { m_shader_editor.onGUI(*m_compiler); }
	bool hasFocus() override { return m_shader_editor.hasFocus(); }
	bool isOpen() const { return m_shader_editor.m_is_open; }

	StudioApp& m_app;
	ShaderCompiler* m_compiler;
	ShaderEditor m_shader_editor;
};


struct GizmoPlugin LUMIX_FINAL : public WorldEditor::Plugin
{
	void showPointLightGizmo(ComponentUID light)
	{
		RenderScene* scene = static_cast<RenderScene*>(light.scene);
		Universe& universe = scene->getUniverse();

		float range = scene->getLightRange(light.entity);

		Vec3 pos = universe.getPosition(light.entity);
		scene->addDebugSphere(pos, range, 0xff0000ff, 0);
	}


	static Vec3 minCoords(const Vec3& a, const Vec3& b)
	{
		return Vec3(Math::minimum(a.x, b.x),
			Math::minimum(a.y, b.y),
			Math::minimum(a.z, b.z));
	}


	static Vec3 maxCoords(const Vec3& a, const Vec3& b)
	{
		return Vec3(Math::maximum(a.x, b.x),
			Math::maximum(a.y, b.y),
			Math::maximum(a.z, b.z));
	}


	void showGlobalLightGizmo(ComponentUID light)
	{
		RenderScene* scene = static_cast<RenderScene*>(light.scene);
		Universe& universe = scene->getUniverse();
		Vec3 pos = universe.getPosition(light.entity);

		Vec3 dir = universe.getRotation(light.entity).rotate(Vec3(0, 0, 1));
		Vec3 right = universe.getRotation(light.entity).rotate(Vec3(1, 0, 0));
		Vec3 up = universe.getRotation(light.entity).rotate(Vec3(0, 1, 0));

		scene->addDebugLine(pos, pos + dir, 0xff0000ff, 0);
		scene->addDebugLine(pos + right, pos + dir + right, 0xff0000ff, 0);
		scene->addDebugLine(pos - right, pos + dir - right, 0xff0000ff, 0);
		scene->addDebugLine(pos + up, pos + dir + up, 0xff0000ff, 0);
		scene->addDebugLine(pos - up, pos + dir - up, 0xff0000ff, 0);

		scene->addDebugLine(pos + right + up, pos + dir + right + up, 0xff0000ff, 0);
		scene->addDebugLine(pos + right - up, pos + dir + right - up, 0xff0000ff, 0);
		scene->addDebugLine(pos - right - up, pos + dir - right - up, 0xff0000ff, 0);
		scene->addDebugLine(pos - right + up, pos + dir - right + up, 0xff0000ff, 0);

		scene->addDebugSphere(pos - dir, 0.1f, 0xff0000ff, 0);
	}


	void showDecalGizmo(ComponentUID cmp)
	{
		RenderScene* scene = static_cast<RenderScene*>(cmp.scene);
		Universe& universe = scene->getUniverse();
		Vec3 scale = scene->getDecalScale(cmp.entity);
		Matrix mtx = universe.getMatrix(cmp.entity);
		scene->addDebugCube(mtx.getTranslation(),
			mtx.getXVector() * scale.x,
			mtx.getYVector() * scale.y,
			mtx.getZVector() * scale.z,
			0xff0000ff,
			0);
	}


	void showCameraGizmo(ComponentUID cmp)
	{
		RenderScene* scene = static_cast<RenderScene*>(cmp.scene);

		scene->addDebugFrustum(scene->getCameraFrustum(cmp.entity), 0xffff0000, 0);
	}


	bool showGizmo(ComponentUID cmp) override 
	{
		if (cmp.type == CAMERA_TYPE)
		{
			showCameraGizmo(cmp);
			return true;
		}
		if (cmp.type == DECAL_TYPE)
		{
			showDecalGizmo(cmp);
			return true;
		}
		if (cmp.type == POINT_LIGHT_TYPE)
		{
			showPointLightGizmo(cmp);
			return true;
		}
		if (cmp.type == GLOBAL_LIGHT_TYPE)
		{
			showGlobalLightGizmo(cmp);
			return true;
		}
		return false;
	}
};


struct AddTerrainComponentPlugin LUMIX_FINAL : public StudioApp::IAddComponentPlugin
{
	explicit AddTerrainComponentPlugin(StudioApp& _app)
		: app(_app)
	{
	}


	bool createHeightmap(const char* material_path, int size)
	{
		char normalized_material_path[MAX_PATH_LENGTH];
		PathUtils::normalize(material_path, normalized_material_path, lengthOf(normalized_material_path));

		PathUtils::FileInfo info(normalized_material_path);
		StaticString<MAX_PATH_LENGTH> hm_path(info.m_dir, info.m_basename, ".raw");
		FS::OsFile file;
		if (!file.open(hm_path, FS::Mode::CREATE_AND_WRITE))
		{
			g_log_error.log("Editor") << "Failed to create heightmap " << hm_path;
			return false;
		}
		else
		{
			u16 tmp = 0xffff >> 1;
			for (int i = 0; i < size * size; ++i)
			{
				file.write(&tmp, sizeof(tmp));
			}
			file.close();
		}

		if (!file.open(normalized_material_path, FS::Mode::CREATE_AND_WRITE))
		{
			g_log_error.log("Editor") << "Failed to create material " << normalized_material_path;
			PlatformInterface::deleteFile(hm_path);
			return false;
		}

		file.writeText("{ \"shader\" : \"pipelines/terrain/terrain.shd\", \
			\"texture\" : {\"source\" : \"");
		file.writeText(info.m_basename);
		file.writeText(".raw\", \"keep_data\" : true}, \
			\"texture\" : {\"source\" : \"/models/utils/white.tga\", \
			\"u_clamp\" : true, \"v_clamp\" : true, \
			\"min_filter\" : \"point\", \"mag_filter\" : \"point\", \"keep_data\" : true}, \
			\"texture\" : {\"source\" : \"\", \"srgb\" : true}, \
			\"texture\" : {\"source\" : \"\", \"srgb\" : true, \"keep_data\" : true}, \
			\"texture\" : {\"source\" : \"/models/utils/white.tga\", \"srgb\" : true}, \
			\"texture\" : {\"source\" : \"\"}, \
			\"uniforms\" : [\
				{\"name\" : \"detail_texture_distance\", \"float_value\" : 80.0}, \
				{ \"name\" : \"texture_scale\", \"float_value\" : 1.0 }], \
			\"metallic\" : 0.06, \"roughness\" : 0.9, \"alpha_ref\" : 0.3 }"
		);

		file.close();
		return true;
	}


	void onGUI(bool create_entity, bool from_filter) override
	{
		WorldEditor& editor = app.getWorldEditor();

		ImGui::SetNextWindowSize(ImVec2(300, 300));
		if (!ImGui::BeginMenu("Terrain")) return;
		char buf[MAX_PATH_LENGTH];
		AssetBrowser& asset_browser = app.getAssetBrowser();
		bool new_created = false;
		if (ImGui::BeginMenu("New"))
		{
			static int size = 1024;
			ImGui::InputInt("Size", &size);
			if (ImGui::Button("Create"))
			{
				char save_filename[MAX_PATH_LENGTH];
				if (PlatformInterface::getSaveFilename(save_filename, lengthOf(save_filename), "Material\0*.mat\0", "mat"))
				{
					editor.makeRelative(buf, lengthOf(buf), save_filename);
					new_created = createHeightmap(buf, size);
				}
			}
			ImGui::EndMenu();
		}
		bool create_empty = ImGui::Selectable("Empty", false);
		if (asset_browser.resourceList(buf, lengthOf(buf), Material::TYPE, 0) || create_empty || new_created)
		{
			if (create_entity)
			{
				Entity entity = editor.addEntity();
				editor.selectEntities(&entity, 1, false);
			}
			if (editor.getSelectedEntities().empty()) return;
			Entity entity = editor.getSelectedEntities()[0];

			if (!editor.getUniverse()->hasComponent(entity, TERRAIN_TYPE))
			{
				editor.addComponent(TERRAIN_TYPE);
			}

			if (!create_empty)
			{
				auto* prop = Reflection::getProperty(TERRAIN_TYPE, "Material");
				editor.setProperty(TERRAIN_TYPE, -1, *prop, &entity, 1, buf, stringLength(buf) + 1);
			}

			ImGui::CloseCurrentPopup();
		}
		ImGui::EndMenu();
	}


	const char* getLabel() const override { return "Render/Terrain"; }


	StudioApp& app;
};



struct StudioAppPlugin : StudioApp::IPlugin
{
	StudioAppPlugin(StudioApp& app)
		: m_app(app)
	{
		IAllocator& allocator = app.getWorldEditor().getAllocator();

		app.registerComponent("camera", "Render/Camera");
		app.registerComponent("global_light", "Render/Global light");

		app.registerComponentWithResource("renderable", "Render/Mesh", Model::TYPE, *Reflection::getProperty(MODEL_INSTANCE_TYPE, "Source"));
		app.registerComponentWithResource("particle_emitter", "Render/Particle emitter/Emitter", Material::TYPE, *Reflection::getProperty(PARTICLE_EMITTER_TYPE, "Material"));
		app.registerComponentWithResource("scripted_particle_emitter", "Render/Particle emitter/DO NOT USE YET! Scripted Emitter", Material::TYPE, *Reflection::getProperty(SCRIPTED_PARTICLE_EMITTER_TYPE, "Material"));
		app.registerComponent("particle_emitter_spawn_shape", "Render/Particle emitter/Spawn shape");
		app.registerComponent("particle_emitter_alpha", "Render/Particle emitter/Alpha");
		app.registerComponent("particle_emitter_plane", "Render/Particle emitter/Plane");
		app.registerComponent("particle_emitter_force", "Render/Particle emitter/Force");
		app.registerComponent("particle_emitter_attractor", "Render/Particle emitter/Attractor");
		app.registerComponent("particle_emitter_subimage", "Render/Particle emitter/Subimage");
		app.registerComponent("particle_emitter_linear_movement", "Render/Particle emitter/Linear movement");
		app.registerComponent("particle_emitter_random_rotation", "Render/Particle emitter/Random rotation");
		app.registerComponent("particle_emitter_size", "Render/Particle emitter/Size");
		app.registerComponent("point_light", "Render/Point light");
		app.registerComponent("decal", "Render/Decal");
		app.registerComponent("bone_attachment", "Render/Bone attachment");
		app.registerComponent("environment_probe", "Render/Environment probe");
		app.registerComponentWithResource("text_mesh", "Render/Text 3D", FontResource::TYPE, *Reflection::getProperty(TEXT_MESH_TYPE, "Font"));

		m_add_terrain_plugin = LUMIX_NEW(allocator, AddTerrainComponentPlugin)(app);
		app.registerComponent("terrain", *m_add_terrain_plugin);

		m_model_plugin = LUMIX_NEW(allocator, ModelPlugin)(app);
		m_material_plugin = LUMIX_NEW(allocator, MaterialPlugin)(app);
		m_font_plugin = LUMIX_NEW(allocator, FontPlugin)(app);
		m_texture_plugin = LUMIX_NEW(allocator, TexturePlugin)(app);
		m_shader_plugin = LUMIX_NEW(allocator, ShaderPlugin)(app);
		AssetBrowser& asset_browser = app.getAssetBrowser();
		asset_browser.addPlugin(*m_model_plugin);
		asset_browser.addPlugin(*m_material_plugin);
		asset_browser.addPlugin(*m_font_plugin);
		asset_browser.addPlugin(*m_texture_plugin);
		asset_browser.addPlugin(*m_shader_plugin);

		m_emitter_plugin = LUMIX_NEW(allocator, EmitterPlugin)(app);
		m_env_probe_plugin = LUMIX_NEW(allocator, EnvironmentProbePlugin)(app);
		m_terrain_plugin = LUMIX_NEW(allocator, TerrainPlugin)(app);
		PropertyGrid& property_grid = app.getPropertyGrid();
		property_grid.addPlugin(*m_emitter_plugin);
		property_grid.addPlugin(*m_env_probe_plugin);
		property_grid.addPlugin(*m_terrain_plugin);

		m_scene_view = LUMIX_NEW(allocator, SceneView)(app);
		m_game_view = LUMIX_NEW(allocator, GameView)(app);
		m_import_asset_dialog = LUMIX_NEW(allocator, ImportAssetDialog)(app);
		m_editor_ui_render_plugin = LUMIX_NEW(allocator, EditorUIRenderPlugin)(app, *m_scene_view, *m_game_view);
		m_fur_painter_plugin = LUMIX_NEW(allocator, FurPainterPlugin)(app);
		m_render_stats_plugin = LUMIX_NEW(allocator, RenderStatsPlugin)(app);
		m_shader_editor_plugin = LUMIX_NEW(allocator, ShaderEditorPlugin)(app);
		app.addPlugin(*m_scene_view);
		app.addPlugin(*m_game_view);
		app.addPlugin(*m_import_asset_dialog);
		app.addPlugin(*m_editor_ui_render_plugin);
		app.addPlugin(*m_fur_painter_plugin);
		app.addPlugin(*m_render_stats_plugin);
		app.addPlugin(*m_shader_editor_plugin);

		m_gizmo_plugin = LUMIX_NEW(allocator, GizmoPlugin)();
		app.getWorldEditor().addPlugin(*m_gizmo_plugin);
	}


	~StudioAppPlugin()
	{
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();

		AssetBrowser& asset_browser = m_app.getAssetBrowser();
		asset_browser.removePlugin(*m_model_plugin);
		asset_browser.removePlugin(*m_material_plugin);
		asset_browser.removePlugin(*m_font_plugin);
		asset_browser.removePlugin(*m_texture_plugin);
		asset_browser.removePlugin(*m_shader_plugin);

		LUMIX_DELETE(allocator, m_model_plugin);
		LUMIX_DELETE(allocator, m_material_plugin);
		LUMIX_DELETE(allocator, m_font_plugin);
		LUMIX_DELETE(allocator, m_texture_plugin);
		LUMIX_DELETE(allocator, m_shader_plugin);

		PropertyGrid& property_grid = m_app.getPropertyGrid();

		property_grid.removePlugin(*m_emitter_plugin);
		property_grid.removePlugin(*m_env_probe_plugin);
		property_grid.removePlugin(*m_terrain_plugin);

		LUMIX_DELETE(allocator, m_emitter_plugin);
		LUMIX_DELETE(allocator, m_env_probe_plugin);
		LUMIX_DELETE(allocator, m_terrain_plugin);

		m_app.removePlugin(*m_scene_view);
		m_app.removePlugin(*m_game_view);
		m_app.removePlugin(*m_import_asset_dialog);
		m_app.removePlugin(*m_editor_ui_render_plugin);
		m_app.removePlugin(*m_fur_painter_plugin);
		m_app.removePlugin(*m_render_stats_plugin);
		m_app.removePlugin(*m_shader_editor_plugin);

		LUMIX_DELETE(allocator, m_scene_view);
		LUMIX_DELETE(allocator, m_game_view);
		LUMIX_DELETE(allocator, m_import_asset_dialog);
		LUMIX_DELETE(allocator, m_editor_ui_render_plugin);
		LUMIX_DELETE(allocator, m_fur_painter_plugin);
		LUMIX_DELETE(allocator, m_render_stats_plugin);
		LUMIX_DELETE(allocator, m_shader_editor_plugin);

		m_app.getWorldEditor().removePlugin(*m_gizmo_plugin);
		LUMIX_DELETE(allocator, m_gizmo_plugin);
	}


	StudioApp& m_app;
	AddTerrainComponentPlugin* m_add_terrain_plugin;
	ModelPlugin* m_model_plugin;
	MaterialPlugin* m_material_plugin;
	FontPlugin* m_font_plugin;
	TexturePlugin* m_texture_plugin;
	ShaderPlugin* m_shader_plugin;
	EmitterPlugin* m_emitter_plugin;
	EnvironmentProbePlugin* m_env_probe_plugin;
	TerrainPlugin* m_terrain_plugin;
	SceneView* m_scene_view;
	GameView* m_game_view;
	ImportAssetDialog* m_import_asset_dialog;
	EditorUIRenderPlugin* m_editor_ui_render_plugin;
	FurPainterPlugin* m_fur_painter_plugin;
	RenderStatsPlugin* m_render_stats_plugin;
	ShaderEditorPlugin* m_shader_editor_plugin;
	GizmoPlugin* m_gizmo_plugin;
};


LUMIX_STUDIO_ENTRY(renderer)
{
	auto& allocator = app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}
