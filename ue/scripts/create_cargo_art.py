"""Build original WP-K1d cargo art (centimetres), materials, sockets and previews.

From repo root, acquire out/locks/ue.lock atomically; release it in finally.
Reference captures (offscreen editor; automatically exits after 90 warm ticks):
  & "$env:USERPROFILE/UnrealEngine-5.9-src/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
    "$PWD/ue/SCViewer/SCViewer.uproject" "-ExecutePythonScript=$PWD/ue/scripts/create_cargo_art.py" `
    -CargoArtWarmPreview -RenderOffscreen -DisablePlugins=DLSS -unattended -nop4 -nosplash `
    -ddc=SCViewerDDC "-ShaderWorkingDir=$PWD/out/k1d/shaders"
Commandlet generation (immediate captures; no warm frames):
  & "$env:USERPROFILE/UnrealEngine-5.9-src/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
    "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript `
    "-script=$PWD/ue/scripts/create_cargo_art.py" -unattended -nop4 -nosplash `
    -AllowCommandletRendering -EnablePlugins=GeometryScripting -ddc=SCViewerDDC `
    "-ShaderWorkingDir=$PWD/out/k1d/shaders"
Only /SCBridge/Cargo assets are saved. Preview world is transient; images and
validation report go to out/k1d. Re-runs update existing assets in place.
No game geometry/textures are used. Dimensions and palette are original art.
"""
import json
import math
from pathlib import Path

import unreal as u

ROOT = "/SCBridge/Cargo"
OUT = Path(__file__).resolve().parents[2] / "out/k1d"
OUT.mkdir(parents=True, exist_ok=True)
EAL, MEL = u.EditorAssetLibrary, u.MaterialEditingLibrary
TOOLS = u.AssetToolsHelpers.get_asset_tools()
SLOTS = ["Paint", "Trim", "Metal", "Emitter", "Lights"]
FANS = [(28, -29, 2), (28, 29, 2), (-28, -29, 2), (-28, 29, 2)]
DRONE_SOCKETS = {**{f"Fan_{i}": (p, (0, 0, 0)) for i, p in enumerate(FANS)},
                 "Tractor": ((0, 0, -19), (-90, 0, 0)),
                 "Dock": ((0, 0, -20), (0, 0, 0)),
                 "Light_Port": ((23, -12.6, 7), (0, -90, 0)),
                 "Light_Starboard": ((23, 12.6, 7), (0, 90, 0)),
                 "Light_Status": ((-22, 0, 14), (90, 0, 0))}
# WP-K4a round 2 (owner: the Fab drone at about 90 cm, one bay per cell side by side): each bay opens on the dock's
# long +Y side. Bay_i is where the drone's centre bone rests (its face ~10 cm behind the opening's lip at 1.9x);
# BayExit_i is 82.5 cm beyond the opening, so the drone's tail clears the roof edge by ~20 cm before it climbs (round-2
# review, out/k4a/astra/r2final.md; the drone leaves along Bay_i -> BayExit_i, and the runtime faces it that way);
# BayLight_i is a neutral fill under the roof's front edge.
DOCK_SOCKETS = {**{f"Bay_{i}": ((x, 7, 57), (0, 90, 0)) for i, x in enumerate((-62.5, 62.5))},
                **{f"BayExit_{i}": ((x, 145, 57), (0, 90, 0)) for i, x in enumerate((-62.5, 62.5))},
                **{f"BayLight_{i}": ((x, 50, 112), (0, 0, 0)) for i, x in enumerate((-62.5, 62.5))}}


def asset(name, folder, cls, factory):
    return u.load_asset(folder + "/" + name) if EAL.does_asset_exist(folder + "/" + name) else TOOLS.create_asset(name, folder, cls, factory)


class Graph:
    def __init__(self, name, blend=u.BlendMode.BLEND_OPAQUE):
        self.m = asset(name, ROOT + "/Materials", u.Material, u.MaterialFactoryNew())
        MEL.delete_all_material_expressions(self.m)
        self.m.set_editor_property("blend_mode", blend)
        self.m.set_editor_property("two_sided", blend != u.BlendMode.BLEND_OPAQUE)
        self.m.set_editor_property("used_with_instanced_static_meshes", True)
        self.row = 0

    def node(self, cls, **props):
        self.row += 1
        n = MEL.create_material_expression(self.m, cls, -900 + (self.row % 4) * 220, (self.row // 4) * 180)
        for k, v in props.items():
            n.set_editor_property(k, v)
        return n

    def link(self, a, b, pin, output=""):
        if not MEL.connect_material_expressions(a, output, b, pin):
            raise RuntimeError(f"{self.m.get_name()}: cannot connect {pin}")

    def scalar(self, name, value):
        return self.node(u.MaterialExpressionScalarParameter, parameter_name=name, default_value=value)

    def colour(self, name, value):
        return self.node(u.MaterialExpressionVectorParameter, parameter_name=name, default_value=u.LinearColor(*value))

    def multiply(self, a, b):
        n = self.node(u.MaterialExpressionMultiply)
        self.link(a, n, "A"); self.link(b, n, "B")
        return n

    def finish(self, bsdf):
        if not MEL.connect_material_property(bsdf, "", u.MaterialProperty.MP_FRONT_MATERIAL):
            raise RuntimeError("Substrate output connection failed")
        MEL.layout_material_expressions(self.m)
        MEL.recompile_material(self.m)
        EAL.save_loaded_asset(self.m)
        return self.m


def materials():
    result = []
    for name, colour, rough, metal in [("Paint", (.64, .36, .045), .38, False),
                                      ("Trim", (.025, .031, .036), .72, False),
                                      ("Metal", (.38, .43, .46), .28, True)]:
        g = Graph("M_Cargo" + name)
        slab = g.node(u.MaterialExpressionSubstrateSlabBSDF)
        c = g.colour("BaseColor", colour)
        g.link(c, slab, "F0" if metal else "Diffuse Albedo", "RGB")
        if metal:
            g.link(g.colour("DiffuseAlbedo", (.015, .018, .02)), slab, "Diffuse Albedo", "RGB")
        g.link(g.scalar("Roughness", rough), slab, "Roughness")
        result.append(g.finish(slab))
    for name in ("M_CargoEmitter", "M_TractorBeam", "M_GridSlot"):
        beam, grid = name == "M_TractorBeam", name == "M_GridSlot"
        g = Graph(name, u.BlendMode.BLEND_ADDITIVE if beam else u.BlendMode.BLEND_TRANSLUCENT if grid else u.BlendMode.BLEND_OPAQUE)
        col = g.colour("Color" if grid or beam else "EmissiveColor", (.12, .58, .85))
        intensity = g.scalar("Opacity" if grid else "Intensity", .8 if grid else 2.5 if beam else 5.)
        emission = g.multiply(col, intensity)
        if beam:
            uv, time = g.node(u.MaterialExpressionTextureCoordinate), g.node(u.MaterialExpressionTime)
            pins=[]
            for pin_name in ("UV", "Time"):
                pin=u.CustomInput()
                pin.set_editor_property("input_name",pin_name)
                pins.append(pin)
            custom = g.node(u.MaterialExpressionCustom,
                            code="float v=UV.y; return saturate(v*10)*saturate((1-v)*10)*(0.12+0.25*pow(0.5+0.5*cos((v*12-Time*1.4)*6.2831853),8));",
                            output_type=u.CustomMaterialOutputType.CMOT_FLOAT1,
                            inputs=pins)
            g.link(uv, custom, "UV"); g.link(time, custom, "Time")
            emission = g.multiply(emission, custom)
        bsdf = g.node(u.MaterialExpressionSubstrateUnlitBSDF)
        g.link(emission, bsdf, "EmissiveColor")
        if grid:
            transmission=g.node(u.MaterialExpressionOneMinus)
            g.link(intensity,transmission,"Input")
            g.link(transmission,bsdf,"TransmittanceColor")
        mat = g.finish(bsdf)
        result.append(mat)
    instances = []
    for master in result:
        mi = asset("MI" + master.get_name()[1:], ROOT + "/Materials", u.MaterialInstanceConstant, u.MaterialInstanceConstantFactoryNew())
        MEL.set_material_instance_parent(mi, master)
        EAL.save_loaded_asset(mi)
        instances.append(mi)
    # Five contract slots; Lights can be changed independently at component level.
    return [instances[0], instances[1], instances[2], instances[3], instances[3]], instances[4], instances[5]


class Mesh:
    """Explicit hard-surface faces -> Geometry Script buffers, grouped by slot."""
    def __init__(self):
        self.faces = [[] for _ in SLOTS]

    def face(self, points, mat=0, uv=None, normals=None):
        # Collapse coincident lathe-pole corners before deriving a face normal.
        # A quad with two identical leading vertices otherwise gets a zero normal.
        unique=[]
        tex=[]
        norms=[]
        for i,p in enumerate(points):
            if not unique or p!=unique[-1]:
                unique.append(p)
                if uv: tex.append(uv[i])
                if normals: norms.append(normals[i])
        if len(unique)>1 and unique[0]==unique[-1]:
            unique.pop()
            if uv: tex.pop()
            if normals: norms.pop()
        if len(unique)>=3:
            self.faces[mat].append((unique,tex if uv else None,norms if normals else None))

    def rings(self, rings, mat=0, cap=True):
        n = len(rings[0])
        for a, b in zip(rings, rings[1:]):
            for i in range(n):
                j = (i + 1) % n
                self.face([a[i], a[j], b[j], b[i]], mat)
        if cap:
            self.face(list(reversed(rings[0])), mat)
            self.face(rings[-1], mat)

    def box(self, p, size, mat=0, bevel=1, yaw=0):
        x, y, z = [v / 2 for v in size]
        b = min(bevel, x * .45, y * .45, z * .45)
        def ring(w, h, c, height):
            pts = [(w-c,-h),(w,-h+c),(w,h-c),(w-c,h),(-w+c,h),(-w,h-c),(-w,-h+c),(-w+c,-h)]
            co, si = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
            return [(p[0]+a*co-d*si, p[1]+a*si+d*co, p[2]+height) for a,d in pts]
        self.rings([ring(x-b,y-b,b,-z), ring(x,y,b,-z+b), ring(x,y,b,z-b), ring(x-b,y-b,b,z)], mat)

    def lathe(self, p, profile, mat=0, n=48, closed=True):
        profile=list(profile)
        if closed:
            profile.append(profile[0])
        for (r0,z0),(r1,z1) in zip(profile,profile[1:]):
            dr,dz=r1-r0,z1-z0
            length=math.hypot(dr,dz)
            for i in range(n):
                a,b=2*math.pi*i/n,2*math.pi*(i+1)/n
                def point(r,z,t): return (p[0]+r*math.cos(t),p[1]+r*math.sin(t),p[2]+z)
                def normal(t): return (dz*math.cos(t)/length,dz*math.sin(t)/length,-dr/length)
                self.face([point(r0,z0,a),point(r0,z0,b),point(r1,z1,b),point(r1,z1,a)],mat,
                          normals=[normal(a),normal(b),normal(b),normal(a)])

    def save(self, name, mats, sockets=None):
        dm = u.DynamicMesh()
        triangles, mins, maxs = 0, [float('inf')]*3, [float('-inf')]*3
        for slot, faces in enumerate(self.faces):
            if not faces:
                continue
            verts, normals, uvs, tris = [], [], [], []
            for pts, uv, smooth in faces:
                base = len(verts)
                a, b, c = [u.Vector(*v) for v in pts[:3]]
                cross=(b-a).cross(c-a)
                length=math.sqrt(cross.x**2+cross.y**2+cross.z**2)
                assert length>0,f'Degenerate face: {pts}'
                # Do not apply FVector's gameplay-sized safe-normal tolerance
                # to sub-millimetre bevel triangles.
                normal=u.Vector(cross.x/length,cross.y/length,cross.z/length)
                verts.extend(u.Vector(*v) for v in pts)
                normals.extend([u.Vector(*v) for v in smooth] if smooth else [normal]*len(pts))
                uvs.extend(u.Vector2D(*v) for v in (uv or [(q[0]*.01,q[1]*.01) for q in pts]))
                # UE GeometryCore/VectorUtil.h: NormalDirection reverses the cross
                # product for the left-handed system. Keep outward normals and
                # emit clockwise indices; CCW indices render the inside faces.
                for i in range(1,len(pts)-1):
                    tri=u.IntVector(base,base+i+1,base+i)
                    ue_normal=(verts[tri.z]-verts[tri.x]).cross(verts[tri.y]-verts[tri.x])
                    assert ue_normal.dot(normal)>=-1e-6,'Triangle winding disagrees with outward normal'
                    tris.append(tri)
                for point in pts:
                    mins = [min(a,b) for a,b in zip(mins,point)]
                    maxs = [max(a,b) for a,b in zip(maxs,point)]
            buf = u.GeometryScriptSimpleMeshBuffers(vertices=verts, normals=normals, uv0=uvs, triangles=tris)
            u.GeometryScript_MeshEdits.append_buffers_to_mesh(dm, buf, material_id=slot)
            triangles += len(tris)
        path=ROOT+"/Meshes/"+name
        if EAL.does_asset_exist(path):
            sm=u.load_asset(path)
        else:
            sm,created=u.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
                dm,path,u.GeometryScriptCreateNewStaticMeshAssetOptions(enable_collision=False))
            if created != u.GeometryScriptOutcomePins.SUCCESS:
                raise RuntimeError(f"Failed to create {name}: {created}")
        opts = u.GeometryScriptCopyMeshToAssetOptions(enable_recompute_tangents=True, replace_materials=True,
                new_materials=mats, new_material_slot_names=SLOTS if len(mats)==5 else ["Surface"])
        _, outcome = u.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(dm, sm, opts, u.GeometryScriptMeshWriteLOD())
        if outcome != u.GeometryScriptOutcomePins.SUCCESS:
            raise RuntimeError(f"Failed to build {name}: {outcome}")
        for socket_name, (pos, rot) in (sockets or {}).items():
            old=sm.find_socket(socket_name)
            if old:
                sm.remove_socket(old)
            s = u.new_object(u.StaticMeshSocket, outer=sm)
            s.set_editor_property("socket_name", socket_name)
            s.set_editor_property("relative_location", u.Vector(*pos))
            s.set_editor_property("relative_rotation", u.Rotator(pitch=rot[0],yaw=rot[1],roll=rot[2]))
            sm.add_socket(s)
        EAL.save_loaded_asset(sm)
        bounds=sm.get_bounding_box()
        actual_min=[bounds.min.x,bounds.min.y,bounds.min.z]
        actual_max=[bounds.max.x,bounds.max.y,bounds.max.z]
        assert all(abs(a-b)<.001 for a,b in zip(actual_min+actual_max,mins+maxs)),name
        if len(mats)==5:
            assert [sm.get_material_index(n) for n in SLOTS]==list(range(5)),name
        for socket_name,(pos,rot) in (sockets or {}).items():
            s=sm.find_socket(socket_name)
            assert s,socket_name
            p=s.relative_location
            assert all(abs(a-b)<.001 for a,b in zip((p.x,p.y,p.z),pos)),socket_name
            r=s.relative_rotation
            assert all(abs(a-b)<.001 for a,b in zip((r.pitch,r.yaw,r.roll),rot)),socket_name
        REPORT[name] = dict(triangles=triangles, render_triangles=sm.get_num_triangles(0),
                            minimum=actual_min, maximum=actual_max, sockets=sockets or {})
        return sm


def drone():
    m = Mesh()
    m.box((0,0,-1),(76,25,21),1,3)
    m.box((0,0,3),(70,24,22),0,3)
    m.box((0,0,14),(39,20,2),2,.5)
    for x in (-28,28):
        m.box((x,0,-2),(10,68,9),2,2)
    for x,y,z in FANS:
        m.lathe((x,y,0),[(17,-8),(19,-8),(20,-6),(20,7),(18.8,9),(16,9),(15,7),(15,-6)],0)
        m.lathe((x,y,0),[(19.5,-4),(20.15,-4),(20.15,0),(19.5,0)],1)
        m.lathe((x,y,0),[(15.1,5),(15.8,5),(15.8,7),(15.1,7)],2)
        for angle in (0,120,240):
            a=math.radians(angle)
            m.box((x+8*math.cos(a),y+8*math.sin(a),-5),(16,1.2,1.2),2,.2,angle)
    for x in range(-13,18,5):
        m.box((x,0,15.2),(2,14,.8),1,.15)
    for x in (-27,27):
        for y in (-8,8):
            m.lathe((x,y,14),[(0,.1),(1.1,.1),(1.1,.6),(0,.6)],2,n=12)
    # Belly emitter, concentric armoured bezel, landing/contact rails.
    m.lathe((0,0,0),list(reversed([(0,-11),(10,-11),(12,-13),(12,-16),(9,-19),(6,-19),(6,-17),(0,-17)])),2)
    m.lathe((0,0,0),[(0,-19),(6,-19),(6,-18.5),(0,-18.5)],3)
    for y in (-10,10):
        m.box((0,y,-17),(34,3,6),1,.8)
    for y in (-12.3,12.3):
        m.box((23,y,7),(8,.6,2.4),4,.15)
    m.box((-22,0,14),(5,8,.8),4,.15)
    # Front sensor inset and hazard ticks: real geometry, no texture dependency.
    m.box((35.2,0,4),(1,16,7),1,.3)
    m.box((35.8,0,4),(.5,9,2),3,.1)
    for x in (-29,-25,-21,21,25,29):
        m.box((x,0,14.2),(1.8,13,.4),1,.1,20)
    return m


def rotor():
    m=Mesh()
    m.lathe((0,0,0),[(0,-2),(3,-2),(4,0),(3,2),(0,2)],2,n=32)
    for i in range(7):
        a=math.radians(i*360/7)
        pts=[(3,-1,-.3),(13,-2,-.3),(14,1,.4),(5,2,1)]
        m.face([(x*math.cos(a)-y*math.sin(a),x*math.sin(a)+y*math.cos(a),z) for x,y,z in pts],1)
        m.face([(x*math.cos(a)-y*math.sin(a),x*math.sin(a)+y*math.cos(a),z-.5) for x,y,z in reversed(pts)],1)
    return m


def dock():
    m=Mesh()
    # Two cells (250 x 125 x 125), one bay per cell side by side, each open on the long +Y side and holding one drone
    # of about 0.9-1 m (the Fab drone at 1.85-2x: ~95 x 80 x 84 cm) facing its opening. Original art: a yellow frame
    # around dark bays, a closed back with a charging spine, a divider between the bays, a cradle under each drone.
    W,D,H=250,125,125
    # Floor plate and roof; frame rails round both.
    m.box((0,0,1.5),(W-4,D-4,3),1,.4)
    m.box((0,0,H-2),(W-4,D-4,4),0,.6)
    for z,h in ((1.5,3),(H-2,4)):
        for y in (-D/2+3,D/2-3):
            m.box((0,y,z),(W,6,h),0,.35)
        for x in (-W/2+3,W/2-3):
            m.box((x,0,z),(6,D,h),0,.35)
    # Corner posts and the front / back posts of the divider.
    for x in (-W/2+4.5,0,W/2-4.5):
        for y in (-D/2+4.5,D/2-4.5):
            m.box((x,y,H/2),(8,8,H-6),0,1.0)
            for z in (9,H-10):
                m.box((x,y,z),(9,9,7),2,.6)
    # Closed back: a dark panel with vertical heat-exchanger fins.
    m.box((0,-D/2+3,H/2),(W-12,4,H-8),2,.4)
    for x in range(-114,115,12):
        m.box((x,-D/2+5.6,H/2),(2.4,1.2,H-34),1,.15)
    # Closed ends; the divider runs from the back to just short of the opening.
    for x in (-W/2+2,W/2-2):
        m.box((x,0,H/2),(4,D-12,H-8),2,.4)
    m.box((0,-4,H/2),(4,D-22,H-8),1,.3)
    for i in range(2):
        (bx,by,bz),_=DOCK_SOCKETS[f"Bay_{i}"]
        # Charging spine on the back panel, facing the drone; its top stands ~10 cm above the drone's body (at 1.9x)
        # with a cyan charge indicator (WP-K4a round 2 review, out/k4a/astra/r2dock.md).
        m.box((bx,-D/2+6.5,66),(30,3,70),1,.6)
        m.box((bx,-D/2+8.2,bz),(18,.6,22),3,.1)
        m.box((bx,-D/2+8.2,95),(2.6,.6,10),3,.1)
        # Cradle: two guide rails along the bay under the drone, with support blocks and contact pads that reach to a few
        # cm under the drone's body (its underside at 1.9x is >= 22.6 cm here, the thruster between the rails lower).
        for x in (bx-18,bx+18):
            m.box((x,by-4,5),(5,70,4),1,.4)
            for y in (by-26,by+18):
                m.box((x,y,13),(6,9,12),1,.4)
                m.box((x,y,19.3),(6,9,.6),3,.1)
        # Threshold: a cyan strip and two chevrons on the floor pointing out of the bay.
        m.box((bx,D/2-7,3.3),(96,1.6,.6),3,.1)
        for y in (D/2-22,D/2-34):
            for side in (-1,1):
                m.box((bx+side*6.5,y,3.4),(16,3,.6),3,.1,-side*40)
        # Bay number ticks on the divider's front post.
        for t in range(i+1):
            m.box((-6 if i==0 else 6,D/2-1,H-24-t*6),(2,.6,3),3,.1)
        # A light bar under the roof's front edge (the BayLight socket sits there).
        m.box((bx,D/2-10,H-5),(70,4,2),4,.3)
    return m


def beam_mesh():
    m=Mesh()
    for j in range(16):
        for i in range(48):
            a,b=2*math.pi*i/48,2*math.pi*(i+1)/48
            m.face([(50*math.cos(a),50*math.sin(a),100*j/16),(50*math.cos(b),50*math.sin(b),100*j/16),
                    (50*math.cos(b),50*math.sin(b),100*(j+1)/16),(50*math.cos(a),50*math.sin(a),100*(j+1)/16)],
                   uv=[(i/48,j/16),((i+1)/48,j/16),((i+1)/48,(j+1)/16),(i/48,(j+1)/16)])
    return m


def grid_mesh():
    m=Mesh()
    for a in (-49.5,49.5):
        for b in (-49.5,49.5):
            m.box((0,a,b+50),(100,1,1),bevel=.05)
            m.box((a,0,b+50),(1,100,1),bevel=.05)
            m.box((a,b,50),(1,1,100),bevel=.05)
    return m


def configured_drone():
    """The skeletal drone Config/Cargo/drone_art.json names, if this machine has it: (mesh, scale, yaw offset, centre
    bone position). Previews only; the asset name stays in the config."""
    path=Path(__file__).resolve().parents[1]/'SCViewer/Config/Cargo/drone_art.json'
    try:
        art=json.loads(path.read_text(encoding='utf-8')).get('art')
    except (OSError, ValueError):
        return None
    if not art or not EAL.does_asset_exist(art['mesh'].split('.')[0]):
        return None
    mesh=u.load_asset(art['mesh'].split('.')[0])
    pose=u.AnimPoseExtensions.get_reference_pose(mesh.skeleton)
    t=u.AnimPoseExtensions.get_ref_bone_pose(pose,art.get('centre_bone','root'),u.AnimPoseSpaces.WORLD).translation
    scale=float(art.get('scale',1))
    for arg in u.SystemLibrary.get_command_line().split():
        if arg.startswith('-CargoArtDroneScale='):
            scale=float(arg.split('=',1)[1])  # previews at another scale (choosing it)
    return mesh,scale,float(art.get('yaw_offset_deg',0)),(t.x,t.y,t.z)


def previews(meshes, mats):
    world=u.EditorLoadingAndSavingUtils.new_blank_map(False)
    actors=u.get_editor_subsystem(u.EditorActorSubsystem)
    def spawn(mesh, pos, scale=(1,1,1), rotation=(0,0,0), material=None):
        a=actors.spawn_actor_from_class(u.StaticMeshActor,u.Vector(*pos),u.Rotator(pitch=rotation[0],yaw=rotation[1],roll=rotation[2]))
        a.static_mesh_component.set_static_mesh(mesh)
        a.set_actor_scale3d(u.Vector(*scale))
        if material: a.static_mesh_component.set_material(0,material)
        return a
    def vehicle(pos):
        spawn(meshes['drone'],pos)
        for p in FANS: spawn(meshes['rotor'],tuple(a+b for a,b in zip(pos,p)))
    spawn(u.load_asset('/Engine/BasicShapes/Cube'),(0,0,-5),(20,20,.1),material=mats[1])
    vehicle((120,-120,65))
    spawn(meshes['dock'],(0,110,0))
    art=configured_drone()
    for i in range(2):
        p,_=DOCK_SOCKETS[f'Bay_{i}']
        if art:
            # The drone drone_art.json names (when installed), at its scale, centre bone on the socket, facing +Y (out).
            mesh,scale,yaw_offset,centre=art
            a=actors.spawn_actor_from_class(u.SkeletalMeshActor,u.Vector(p[0],110+p[1]-centre[1]*scale,p[2]-centre[2]*scale),u.Rotator(pitch=0,yaw=90+yaw_offset,roll=0))
            a.skeletal_mesh_component.set_skeletal_mesh_asset(mesh)
            a.set_actor_scale3d(u.Vector(scale,scale,scale))
        else:
            vehicle((p[0],110+p[1],p[2]))
    vehicle((-120,-120,190))
    spawn(meshes['beam'],(-120,-120,171),(.12,.12,1.5),(0,0,180))
    spawn(meshes['grid'],(-120,-120,0),(1.25,1.25,1.25))
    reference=spawn(u.load_asset('/Engine/BasicShapes/Cube'),(-190,110,62.5),(1.25,1.25,1.25),material=mats[0])
    reference_material=reference.static_mesh_component.create_dynamic_material_instance(0)
    reference_material.set_vector_parameter_value('BaseColor',u.LinearColor(.24,.27,.29))
    reference_material.set_scalar_parameter_value('Roughness',.8)
    spawn(meshes['grid'],(-190,110,0),(1.25,1.25,1.25))
    for light_index,(pos,rot,intensity) in enumerate([((200,-250,450),(-55,135,0),8),((-200,100,350),(-45,-45,0),5),
                              ((0,0,300),(-25,45,0),3),((0,0,300),(-25,-135,0),3)]):
        a=actors.spawn_actor_from_class(u.DirectionalLight,u.Vector(*pos),u.Rotator(pitch=rot[0],yaw=rot[1],roll=rot[2]))
        a.light_component.set_intensity(intensity)
        a.light_component.set_editor_property('cast_shadows',light_index==0)
    sky=actors.spawn_actor_from_class(u.SkyLight,u.Vector(0,0,300))
    sky.light_component.set_editor_property('source_type',u.SkyLightSourceType.SLS_SPECIFIED_CUBEMAP)
    sky.light_component.set_cubemap(u.load_asset('/Engine/MapTemplates/Sky/DaylightAmbientCubemap'))
    sky.light_component.set_intensity(1.5)
    pp=u.PostProcessSettings()
    pp.set_editor_property('override_auto_exposure_method',True)
    pp.set_editor_property('auto_exposure_method',u.AutoExposureMethod.AEM_MANUAL)
    pp.set_editor_property('override_auto_exposure_apply_physical_camera_exposure',True)
    pp.set_editor_property('auto_exposure_apply_physical_camera_exposure',False)
    pp.set_editor_property('override_auto_exposure_bias',True)
    pp.set_editor_property('auto_exposure_bias',0)
    pp.set_editor_property('override_ambient_cubemap_intensity',True)
    pp.set_editor_property('ambient_cubemap_intensity',1)
    pp.set_editor_property('ambient_cubemap',u.load_asset('/Engine/MapTemplates/Sky/DaylightAmbientCubemap'))
    pp.set_editor_property('override_bloom_intensity',True)
    pp.set_editor_property('bloom_intensity',0.15)
    u.SystemLibrary.execute_console_command(world,'r.ShaderPipelineCache.Enabled 0')
    shots=[]
    for name,eye,target in [('overview',(510,-670,420),(-20,0,65)),
                            ('drone',(300,-340,230),(120,-120,62)),
                            ('dock',(330,480,210),(0,110,55)),
                            ('dock_front',(40,440,95),(0,110,58)),
                            ('dock_bay',(-40,300,70),(-62.5,110,55)),
                            ('beam',(180,-480,240),(-120,-120,95)),
                            ('diagnostic',(300,-340,230),(120,-120,62))]:
        cap=actors.spawn_actor_from_class(u.SceneCapture2D,u.Vector(*eye),u.MathLibrary.find_look_at_rotation(u.Vector(*eye),u.Vector(*target)))
        c=cap.get_component_by_class(u.SceneCaptureComponent2D)
        rt=u.RenderingLibrary.create_render_target2d(world,1600,1100,u.TextureRenderTargetFormat.RTF_RGBA8)
        c.set_editor_property('texture_target',rt)
        c.set_editor_property('capture_source',u.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        if name=='diagnostic':
            c.set_editor_property('capture_source',u.SceneCaptureSource.SCS_BASE_COLOR)
        c.set_editor_property('capture_every_frame',False)
        c.set_editor_property('fov_angle',48)
        c.set_editor_property('post_process_settings',pp)
        c.set_editor_property('always_persist_rendering_state',True)
        shots.append((c,rt,name))
    if '-CargoArtWarmPreview' in u.SystemLibrary.get_command_line():
        u.EditorPythonScripting.set_keep_python_script_alive(True)
        state={'frame':0}
        def tick(delta):
            state['frame']+=1
            for c,rt,name in shots:
                c.capture_scene()
            if state['frame']==90:
                for c,rt,name in shots:
                    u.RenderingLibrary.export_render_target(world,rt,str(OUT),f'preview_{name}.png')
                u.unregister_slate_post_tick_callback(state['handle'])
                u.EditorPythonScripting.set_keep_python_script_alive(False)
                u.log('K1D_PREVIEWS_COMPLETE')
        state['handle']=u.register_slate_post_tick_callback(tick)
    else:
        for c,rt,name in shots:
            c.capture_scene()
            u.RenderingLibrary.export_render_target(world,rt,str(OUT),f'preview_{name}.png')


REPORT={}
if __name__ == '__main__':
    u.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT],force_rescan=True)
    if not u.SystemLibrary.get_console_variable_bool_value('r.Substrate'):
        raise RuntimeError('Cargo materials require Substrate')
    mats,beam,grid=materials()
    meshes={'drone':drone().save('SM_CargoDrone',mats,DRONE_SOCKETS),
            'rotor':rotor().save('SM_CargoDroneRotor',mats),
            'dock':dock().save('SM_CargoDroneDock',mats,DOCK_SOCKETS),
            'beam':beam_mesh().save('SM_TractorBeam',[beam]),
            'grid':grid_mesh().save('SM_GridSlotFrame',[grid])}
    assert REPORT['SM_CargoDrone']['triangles'] < 30000
    assert all(b-a<=limit for a,b,limit in zip(REPORT['SM_CargoDrone']['minimum'],REPORT['SM_CargoDrone']['maximum'],(100,100,45)))
    assert REPORT['SM_CargoDroneDock']['triangles'] < 30000
    assert REPORT['SM_CargoDroneDock']['minimum']==[-125,-62.5,0]
    assert REPORT['SM_CargoDroneDock']['maximum']==[125,62.5,125]
    (OUT/'validation.json').write_text(json.dumps(REPORT,indent=2))
    previews(meshes,mats)
    u.log('K1D_ART_COMPLETE '+str(OUT))

