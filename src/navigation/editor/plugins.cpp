#include "editor/platform_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/universe/universe.h"
#include "navigation/navigation_scene.h"
#include <DetourCrowd.h>


using namespace Lumix;


namespace
{


struct NavmeshEditorPlugin LUMIX_FINAL : public StudioApp::GUIPlugin
{
	explicit NavmeshEditorPlugin(StudioApp& _app)
		: app(_app)
		, is_open(false)
	{
		IAllocator& allocator = app.getWorldEditor().getAllocator();
		Action* action = LUMIX_NEW(allocator, Action)("Navigation", "Toggle navigation UI", "toggleNavigationWindow");
		action->func.bind<NavmeshEditorPlugin, &NavmeshEditorPlugin::onAction>(this);
		action->is_selected.bind<NavmeshEditorPlugin, &NavmeshEditorPlugin::isOpen>(this);
		app.addWindowAction(action);
	}


	const char* getName() const override { return "navigation"; }


	void onAction()
	{
		is_open = !is_open;
	}


	bool isOpen() const
	{
		return is_open;
	}


	void onWindowGUI() override
	{
		auto* scene = static_cast<NavigationScene*>(app.getWorldEditor().getUniverse()->getScene(crc32("navigation")));
		if (!scene) return;

		if (ImGui::BeginDock("Navigation", &is_open, ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (ImGui::Button("Generate")) scene->generateNavmesh();
			ImGui::SameLine();
			if (ImGui::Button("Load"))
			{
				char path[MAX_PATH_LENGTH];
				if (PlatformInterface::getOpenFilename(path, lengthOf(path), "Navmesh\0*.nav\0", nullptr))
				{
					scene->load(path);
				}
			}
			if (scene->isNavmeshReady())
			{
				ImGui::SameLine();
				if (ImGui::Button("Save"))
				{
					char path[MAX_PATH_LENGTH];
					if (PlatformInterface::getSaveFilename(path, lengthOf(path), "Navmesh\0*.nav\0", nullptr))
					{
						scene->save(path);
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Debug tile"))
				{
					Vec3 camera_hit = app.getWorldEditor().getCameraRaycastHit();
					scene->generateTileAt(camera_hit, true);
				}

				static bool debug_draw_path = false;
				const auto& selected_entities = app.getWorldEditor().getSelectedEntities();
				if (!selected_entities.empty())
				{
					const dtCrowdAgent* agent = scene->getDetourAgent({selected_entities[0].index});
					if (agent)
					{
						ImGui::Text("Agent");
						ImGui::Checkbox("Draw path", &debug_draw_path);
						if (debug_draw_path) scene->debugDrawPath({selected_entities[0].index});
						ImGui::LabelText("Desired speed", "%f", agent->desiredSpeed);
						ImGui::LabelText("Corners", "%d", agent->ncorners);
						if (agent->ncorners > 0)
						{
							Vec3 pos = *(Vec3*)agent->npos;
							Vec3 corner = *(Vec3*)agent->targetPos;

							ImGui::LabelText("Target distance", "%f", (pos - corner).length());
						}

						static const char* STATES[] = { "Invalid", "Walking", "Offmesh" };
						if (agent->state < lengthOf(STATES)) ImGui::LabelText("State", "%s", STATES[agent->state]);
						static const char* TARGET_STATES[] = { "None", "Failed", "Valid", "Requesting", "Waiting for queue", "Waiting for path", "Velocity" };
						if (agent->targetState < lengthOf(TARGET_STATES)) ImGui::LabelText("Target state", "%s", TARGET_STATES[agent->targetState]);
						ImGui::Separator();
					}
				}

				static bool debug_draw_navmesh = false;
				ImGui::Checkbox("Draw navmesh", &debug_draw_navmesh);
				if (debug_draw_navmesh)
				{
					static bool inner_boundaries = true;
					static bool outer_boundaries = true;
					static bool portals = true;
					ImGui::Checkbox("Inner boundaries", &inner_boundaries);
					ImGui::Checkbox("Outer boundaries", &outer_boundaries);
					ImGui::Checkbox("Portals", &portals);
					scene->debugDrawNavmesh(app.getWorldEditor().getCameraRaycastHit(), inner_boundaries, outer_boundaries, portals);
				}

				if (scene->hasDebugDrawData())
				{
					static bool debug_draw_compact_heightfield = false;
					ImGui::Checkbox("Draw compact heightfield", &debug_draw_compact_heightfield);
					if (debug_draw_compact_heightfield) scene->debugDrawCompactHeightfield();

					static bool debug_draw_heightfield = false;
					ImGui::Checkbox("Draw heightfield", &debug_draw_heightfield);
					if (debug_draw_heightfield) scene->debugDrawHeightfield();

					static bool debug_draw_contours = false;
					ImGui::Checkbox("Draw contours", &debug_draw_contours);
					if (debug_draw_contours) scene->debugDrawContours();

					auto& entities = app.getWorldEditor().getSelectedEntities();
					if (!entities.empty())
					{
						static bool debug_draw_path = false;
						ImGui::Checkbox("Draw path", &debug_draw_path);
						if (debug_draw_path) scene->debugDrawPath({entities[0].index});
					}
				}
				else
				{
					ImGui::Text("For more info press \"Debug tile\"");
				}
			}
		}
		ImGui::EndDock();
	}


	bool is_open;
	StudioApp& app;
};


struct StudioAppPlugin : StudioApp::IPlugin
{
	StudioAppPlugin(StudioApp& app)
		: m_app(app)
	{
		IAllocator& allocator = app.getWorldEditor().getAllocator();
		m_navmesh_editor = LUMIX_NEW(allocator, NavmeshEditorPlugin)(app);
		app.addPlugin(*m_navmesh_editor);

		app.registerComponent("navmesh_agent", "Navmesh Agent");
	}


	~StudioAppPlugin()
	{
		m_app.removePlugin(*m_navmesh_editor);
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		LUMIX_DELETE(allocator, m_navmesh_editor);
	}


	StudioApp& m_app;
	NavmeshEditorPlugin* m_navmesh_editor;
};


} // anonymous


LUMIX_STUDIO_ENTRY(navigation)
{
	IAllocator& allocator = app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}

