# SmallRender_blender.py — Farm submitter addon for Blender
# Install via: Edit > Preferences > Add-ons > Install from Disk
#
# Submits render jobs to SmallRender farm via the shared filesystem
# submissions inbox (Tier 2 automated submission).

bl_info = {
    "name": "SmallRender",
    "author": "SmallRender",
    "version": (0, 1, 2),
    "blender": (4, 0, 0),
    "location": "Render Properties > SmallRender",
    "description": "Submit render jobs to SmallRender farm",
    "category": "Render",
}

import bpy
import json
import os
import platform
import socket
import time
from bpy.props import EnumProperty, IntProperty, BoolProperty, StringProperty
from bpy.types import Panel, Operator, PropertyGroup


# ── Config helpers ────────────────────────────────────────────────────────────

def get_config_dir():
    sys = platform.system()
    if sys == "Windows":
        return os.path.join(os.environ.get("LOCALAPPDATA", ""), "SmallRender")
    elif sys == "Darwin":
        return os.path.expanduser("~/Library/Application Support/SmallRender")
    else:
        xdg = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
        return os.path.join(xdg, "SmallRender")


def read_config():
    config_path = os.path.join(get_config_dir(), "config.json")
    if not os.path.isfile(config_path):
        return None
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def get_farm_path():
    config = read_config()
    if not config or not config.get("sync_root"):
        return None
    farm = os.path.join(config["sync_root"], "SmallRender-v1")
    if os.path.isdir(farm):
        return farm
    return None


# ── Template scanning ─────────────────────────────────────────────────────────

_template_cache = []
_template_cache_time = 0.0


def scan_templates():
    global _template_cache, _template_cache_time

    now = time.time()
    if _template_cache and (now - _template_cache_time) < 5.0:
        return _template_cache

    farm = get_farm_path()
    if not farm:
        _template_cache = []
        _template_cache_time = now
        return []

    templates = []
    templates_dir = os.path.join(farm, "templates")

    for search_dir in [templates_dir, os.path.join(templates_dir, "examples")]:
        if not os.path.isdir(search_dir):
            continue
        for fname in sorted(os.listdir(search_dir)):
            if not fname.endswith(".json"):
                continue
            fpath = os.path.join(search_dir, fname)
            try:
                with open(fpath, "r", encoding="utf-8") as f:
                    tpl = json.load(f)
                tid = tpl.get("template_id", "")
                name = tpl.get("name", tid)
                if tid:
                    templates.append((tid, name))
            except Exception:
                continue

    _template_cache = templates
    _template_cache_time = now
    return templates


# Persistent reference to prevent garbage collection (Blender EnumProperty quirk)
_enum_items = []


def get_template_items(self, context):
    global _enum_items
    templates = scan_templates()
    if not templates:
        _enum_items = [("NONE", "No templates found", "")]
        return _enum_items
    _enum_items = [(t[0], t[1], "") for t in templates]
    return _enum_items


# ── Properties ────────────────────────────────────────────────────────────────

def _on_override_range(self, context):
    if self.override_range:
        scene = context.scene
        self.frame_start = scene.frame_start
        self.frame_end = scene.frame_end


def _on_override_output(self, context):
    if self.override_output:
        scene = context.scene
        self.output_path = scene.render.filepath


class SmallRenderSettings(PropertyGroup):
    template: EnumProperty(
        name="Template",
        description="SmallRender job template to use",
        items=get_template_items,
    )
    chunk_size: IntProperty(
        name="Chunk Size",
        description="Number of frames per render chunk",
        default=10,
        min=1,
        max=10000,
    )
    priority: IntProperty(
        name="Priority",
        description="Job priority (higher = rendered first)",
        default=50,
        min=1,
        max=100,
    )
    submit_all_scenes: BoolProperty(
        name="All Scenes",
        description="Submit every scene in the file as a separate job",
        default=False,
    )
    override_range: BoolProperty(
        name="Override Range",
        description="Override the frame range from scene settings",
        default=False,
        update=_on_override_range,
    )
    frame_start: IntProperty(
        name="Start",
        description="Start frame",
        default=1,
        min=0,
    )
    frame_end: IntProperty(
        name="End",
        description="End frame",
        default=250,
        min=0,
    )
    override_output: BoolProperty(
        name="Override Output",
        description="Override the output path from scene settings",
        default=False,
        update=_on_override_output,
    )
    output_path: StringProperty(
        name="Output",
        description="Output path override",
        default="",
        subtype='FILE_PATH',
    )


# ── Sync Operator ────────────────────────────────────────────────────────────

class SMALLRENDER_OT_sync_from_scene(Operator):
    bl_idname = "smallrender.sync_from_scene"
    bl_label = "Sync from Scene"
    bl_description = "Populate override fields from current scene settings"
    bl_options = {'INTERNAL'}

    def execute(self, context):
        settings = context.scene.smallrender
        scene = context.scene
        settings.frame_start = scene.frame_start
        settings.frame_end = scene.frame_end
        settings.output_path = scene.render.filepath
        return {'FINISHED'}


# ── Submit cooldown ──────────────────────────────────────────────────────────

_submit_cooldown_until = 0.0
_SUBMIT_COOLDOWN_SECS = 2.0


# ── Submit Operator ───────────────────────────────────────────────────────────

class SMALLRENDER_OT_submit(Operator):
    bl_idname = "smallrender.submit"
    bl_label = "Submit to SmallRender"
    bl_description = "Submit render job(s) to SmallRender farm"

    def execute(self, context):
        global _submit_cooldown_until
        if time.time() < _submit_cooldown_until:
            self.report({'WARNING'}, "Already submitted — please wait a few seconds.")
            return {'CANCELLED'}
        settings = context.scene.smallrender

        # Validate farm connection
        farm = get_farm_path()
        if not farm:
            config = read_config()
            if not config:
                self.report({'ERROR'}, "SmallRender config not found. Is the monitor installed?")
            elif not config.get("sync_root"):
                self.report({'ERROR'}, "Sync root not set. Configure it in SmallRender Monitor.")
            else:
                self.report({'ERROR'}, "Farm not initialized.")
            return {'CANCELLED'}

        submissions_dir = os.path.join(farm, "submissions")
        if not os.path.isdir(submissions_dir):
            self.report({'ERROR'}, "Submissions folder not found. Is the farm initialized?")
            return {'CANCELLED'}

        if not bpy.data.filepath:
            self.report({'ERROR'}, "Save the file before submitting.")
            return {'CANCELLED'}

        template_id = settings.template
        if template_id == "NONE":
            self.report({'ERROR'}, "No templates available on the farm.")
            return {'CANCELLED'}

        # Save file
        bpy.ops.wm.save_mainfile()

        # Scenes to submit
        if settings.submit_all_scenes:
            scenes = list(bpy.data.scenes)
        else:
            scenes = [context.scene]

        hostname = socket.gethostname()
        blend_name = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
        submitted = 0

        for scene in scenes:
            # Job name
            if len(scenes) > 1:
                job_name = "{} - {}".format(blend_name, scene.name)
            else:
                job_name = blend_name

            # Frame range (override or scene)
            if settings.override_range:
                f_start = settings.frame_start
                f_end = settings.frame_end
            else:
                f_start = scene.frame_start
                f_end = scene.frame_end

            # Output path (override or scene)
            if settings.override_output and settings.output_path:
                out_path = bpy.path.abspath(settings.output_path)
            else:
                out_path = bpy.path.abspath(scene.render.filepath)

            # Overrides (keyed by template flag IDs)
            overrides = {
                "scene_file": bpy.data.filepath,
                "output_path": out_path,
            }

            # Include scene name for multi-scene or non-default scene
            if len(scenes) > 1 or scene.name != "Scene":
                overrides["scene_name"] = scene.name

            ts = int(time.time() * 1000) + submitted

            submission = {
                "_version": 1,
                "template_id": template_id,
                "job_name": job_name,
                "submitted_by_host": hostname,
                "submitted_at_ms": ts,
                "overrides": overrides,
                "frame_start": f_start,
                "frame_end": f_end,
                "chunk_size": settings.chunk_size,
                "priority": settings.priority,
            }

            filename = "{:013d}.{}.json".format(ts, hostname)
            filepath = os.path.join(submissions_dir, filename)

            try:
                tmp_path = filepath + ".tmp"
                with open(tmp_path, "w", encoding="utf-8") as f:
                    json.dump(submission, f, indent=2)
                os.replace(tmp_path, filepath)
                submitted += 1
            except Exception as e:
                self.report({'ERROR'}, "Failed to write submission: {}".format(e))
                return {'CANCELLED'}

        _submit_cooldown_until = time.time() + _SUBMIT_COOLDOWN_SECS
        self.report({'INFO'}, "Submitted {} job{}".format(
            submitted, "s" if submitted != 1 else ""))
        return {'FINISHED'}


# ── Panel ─────────────────────────────────────────────────────────────────────

class SMALLRENDER_PT_main(Panel):
    bl_label = "SmallRender"
    bl_idname = "SMALLRENDER_PT_main"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        settings = context.scene.smallrender
        scene = context.scene

        # Check farm connection
        farm = get_farm_path()
        if not farm:
            layout.label(text="Farm not connected", icon='ERROR')
            config = read_config()
            if not config:
                layout.label(text="SmallRender config not found.")
            elif not config.get("sync_root"):
                layout.label(text="Set sync root in SmallRender Monitor.")
            else:
                layout.label(text="Farm folder not found.")
            return

        # Template selector
        layout.prop(settings, "template")

        layout.separator()

        # Scene info
        box = layout.box()
        if not bpy.data.filepath:
            box.label(text="File not saved", icon='ERROR')
        else:
            box.label(text=os.path.basename(bpy.data.filepath), icon='FILE_BLEND')

        # Frame range
        row = box.row()
        row.prop(settings, "override_range", text="")
        if settings.override_range:
            sub = row.row(align=True)
            sub.prop(settings, "frame_start")
            sub.prop(settings, "frame_end")
            count = settings.frame_end - settings.frame_start + 1
        else:
            row.label(text="Frames: {} - {} ({})".format(
                scene.frame_start, scene.frame_end,
                scene.frame_end - scene.frame_start + 1))
            count = scene.frame_end - scene.frame_start + 1

        # Output path
        row = box.row()
        row.prop(settings, "override_output", text="")
        if settings.override_output:
            row.prop(settings, "output_path", text="")
        else:
            if scene.render.filepath:
                out = scene.render.filepath
                if len(out) > 45:
                    out = "..." + out[-42:]
                row.label(text=out, icon='OUTPUT')
            else:
                row.label(text="No output path set", icon='OUTPUT')

        layout.separator()

        # Settings
        row = layout.row(align=True)
        row.prop(settings, "chunk_size")
        row.prop(settings, "priority")

        layout.prop(settings, "submit_all_scenes")

        if settings.submit_all_scenes and len(bpy.data.scenes) > 1:
            layout.label(text="{} scenes will be submitted".format(len(bpy.data.scenes)))

        layout.separator()

        # Submit button (with cooldown feedback)
        row = layout.row()
        row.scale_y = 1.5
        in_cooldown = time.time() < _submit_cooldown_until
        row.enabled = bool(bpy.data.filepath) and not in_cooldown
        if in_cooldown:
            row.operator("smallrender.submit", text="Submitted!", icon='CHECKMARK')
        else:
            row.operator("smallrender.submit", text="Submit to Farm", icon='RENDER_ANIMATION')


# ── Registration ──────────────────────────────────────────────────────────────

classes = (
    SmallRenderSettings,
    SMALLRENDER_OT_sync_from_scene,
    SMALLRENDER_OT_submit,
    SMALLRENDER_PT_main,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.smallrender = bpy.props.PointerProperty(type=SmallRenderSettings)


def unregister():
    del bpy.types.Scene.smallrender
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
