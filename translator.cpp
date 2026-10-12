#include "translator.h"
#include "rigExecRigging/rigBuilder.h"
#include "rigExecRigging/schemaAuthoring.h"
#include "rigExec/rigEvaluator.h"
#include "pxr/base/js/json.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/plug/plugin.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usd/sdf/copyUtils.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/points.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdSkel/blendShape.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "pxr/usd/usdShade/udimUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <sstream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdBlenderRig {
namespace {
constexpr size_t MaxElements=10000000;
const JsObject &Object(const JsValue &v) {
    if(!v.IsObject()) throw std::runtime_error("expected JSON object");
    return v.GetJsObject();
}
const JsArray &Array(const JsValue &v) {
    if(!v.IsArray() || v.GetJsArray().size()>MaxElements) throw std::runtime_error("invalid JSON array");
    return v.GetJsArray();
}
const JsValue &Field(const JsValue &v,const std::string &key) {
    const auto &obj=Object(v); const auto it=obj.find(key);
    if(it==obj.end()) throw std::runtime_error("missing snapshot field: "+key);
    return it->second;
}
bool Has(const JsValue &v,const std::string &key) { return Object(v).count(key)!=0; }
std::string String(const JsValue &v) {
    if(!v.IsString()) throw std::runtime_error("expected JSON string");
    if(v.GetString().find('\0')!=std::string::npos) throw std::runtime_error("embedded NUL in string");
    return v.GetString();
}
std::string Str(const JsValue &v,const std::string &key) { return String(Field(v,key)); }
double Number(const JsValue &v) {
    double d;
    if(v.IsUInt64()) d=static_cast<double>(v.GetUInt64());
    else if(v.IsInt()) d=static_cast<double>(v.GetInt64());
    else if(v.IsReal()) d=v.GetReal();
    else throw std::runtime_error("expected JSON number");
    if(!std::isfinite(d)) throw std::runtime_error("non-finite number");
    return d;
}
int Integer(const JsValue &v) {
    const double d=Number(v);
    if(std::floor(d)!=d || d<std::numeric_limits<int>::min() || d>std::numeric_limits<int>::max())
        throw std::runtime_error("invalid integer");
    return static_cast<int>(d);
}
float Float(const JsValue &v) {
    const double d=Number(v);
    if(std::abs(d)>std::numeric_limits<float>::max()) throw std::runtime_error("float overflow");
    return static_cast<float>(d);
}
bool Bool(const JsValue &v) {
    if(!v.IsBool()) throw std::runtime_error("expected JSON boolean");
    return v.GetBool();
}
bool Flag(const JsValue &v,const std::string &key,bool fallback) {
    return Has(v,key) ? Bool(Field(v,key)) : fallback;
}
double Num(const JsValue &v,const std::string &key,double fallback) {
    return Has(v,key) ? Number(Field(v,key)) : fallback;
}
GfMatrix4d Matrix(const JsValue &v) {
    const auto &a=Array(v); if(a.size()!=16) throw std::runtime_error("matrix must contain 16 numbers");
    GfMatrix4d result(1);
    for(int i=0;i<16;++i) result[i/4][i%4]=Number(a[i]);
    if(std::abs(result.GetDeterminant())<1e-12 || result[0][3]!=0 || result[1][3]!=0 ||
       result[2][3]!=0 || result[3][3]!=1) throw std::runtime_error("singular or non-affine rest matrix");
    return result;
}
GfVec3f Point(const JsValue &v) {
    const auto &a=Array(v); if(a.size()!=3) throw std::runtime_error("point must contain 3 numbers");
    return GfVec3f(Float(a[0]),Float(a[1]),Float(a[2]));
}
// UV property identifiers remain reversible; prim names need not encode IDs.
std::string PropertyName(const std::string &s) {
    std::string result="n"; const char *hex="0123456789abcdef";
    for(unsigned char c:s) {
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9')) result+=c;
        else { result+='_'; result+=hex[c>>4]; result+=hex[c&15]; }
    }
    return result;
}
std::map<std::string,std::string> PrimNames(const std::map<std::string,std::string> &labels) {
    std::map<std::string,std::vector<std::string>> groups;
    for(const auto &entry:labels) {
        std::string base;
        for(unsigned char c:entry.second) {
            const bool valid=(c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9');
            if(valid) base+=c;
            else if(!base.empty() && base.back()!='_') base+='_' ;
            if(base.size()==32) break;
        }
        while(!base.empty() && base.back()=='_') base.pop_back();
        if(base.empty()) base="node";
        if(base.front()>='0' && base.front()<='9') base="n_"+base;
        groups[base].push_back(entry.first);
    }
    std::map<std::string,std::string> result;
    // Reserve unambiguous labels first so a disambiguation cannot steal one.
    std::set<std::string> used{"EditorFrame","Display"};
    for(const auto &group:groups) if(group.second.size()==1 && !used.count(group.first)) {
        result[group.second.front()]=group.first;used.insert(group.first);
    }
    for(const auto &group:groups) for(const auto &id:group.second) {
        if(result.count(id)) continue;
        uint64_t hash=14695981039346656037ull;
        for(unsigned char c:id) { hash^=c;hash*=1099511628211ull; }
        std::ostringstream suffix;suffix<<std::hex<<std::setw(8)<<std::setfill('0')<<uint32_t(hash);
        const auto stem=group.first+"_"+suffix.str();auto name=stem;
        for(size_t collision=1;!used.insert(name).second;++collision) name=stem+"_"+std::to_string(collision);
        result[id]=name;
    }
    return result;
}
bool Rigid(const GfMatrix4d &m) {
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) {
        double dot=0; for(int k=0;k<3;++k) dot+=m[i][k]*m[j][k];
        if(std::abs(dot-(i==j ? 1.0 : 0.0))>1e-6) return false;
    }
    return m.GetDeterminant()>0;
}
GfMatrix4d PoseMatrix(const JsArray &pose) {
    if(pose.size()!=9) throw std::runtime_error("pose must contain 9 TRS channels");
    GfMatrix4d result(1);result.SetScale(GfVec3d(Number(pose[6]),Number(pose[7]),Number(pose[8])));
    for(int i=0;i<3;++i) {GfVec3d axis(0);axis[i]=1;result*=GfMatrix4d(GfRotation(axis,Number(pose[i+3])),GfVec3d(0));}
    result.SetTranslateOnly(GfVec3d(Number(pose[0]),Number(pose[1]),Number(pose[2])));
    return result;
}
struct Node {
    JsValue data;
    std::string id,parent,kind,name;
    SdfPath path,controlPath;
    // restFrame is the default space RigExec derives for the node: its
    // namespace chain of rest:space values, each orthonormalized on its own.
    GfMatrix4d rest{1},world{1},restFrame{1};
    int state=0;
};
class Importer {
public:
    JsValue scene;
    std::string source;
    UsdStageRefPtr stage=UsdStage::CreateInMemory();
    rigExec::RigExecRigBuilder rig=rigExec::RigExecRigBuilder::Create(stage);
    std::map<std::string,Node> nodes;
    std::map<std::string,UsdShadeMaterial> materials;
    std::map<std::string,double> guideExtents;
    std::vector<std::string> diagnostics,dependencies;
    TfTokenVector operationOrder;
    SdfPath identityParent;
    mutable std::map<std::string,std::vector<size_t>> activeStacks;
    mutable bool activeStacksBuilt=false;
    std::map<std::tuple<std::string,std::string,bool>,SdfPath> skinProviders;
    std::set<std::string> nodeNames;
    std::set<std::string> pruned;
    bool prune=false,wholeProbe=false;
    explicit Importer(JsValue data,std::string origin):scene(std::move(data)),source(std::move(origin)) {}
    void Warn(const std::string &message) { diagnostics.push_back(message); }
    // VtValue equality treats -0.0 as 0.0; a skipped value must also print alike.
    static bool Identical(const VtValue &a,const VtValue &b) {
        if(a!=b) return false;
        if(a.IsHolding<double>()) return std::signbit(a.UncheckedGet<double>())==std::signbit(b.UncheckedGet<double>());
        if(a.IsHolding<bool>() || a.IsHolding<TfToken>() || a.IsHolding<int>()) return true;
        return TfStringify(a)==TfStringify(b);
    }
    // A schema fallback resolves exactly like an authored copy of it, so only
    // differing values get a spec. An earlier opinion is still replaced.
    void Set(const rigExec::RigExecSchemaPrim &schema,const TfToken &name,const VtValue &value) {
        const auto prim=schema.GetPrim(); VtValue fallback;
        if(prim.GetPrimDefinition().GetAttributeFallbackValue(name,&fallback) && Identical(fallback,value) &&
           !prim.GetAttribute(name).HasAuthoredValue()) return;
        schema.SetAttribute(name,value);
    }
    // The one identity matrix that explicit space connections read.
    const SdfPath &IdentitySource() {
        if(identityParent.IsEmpty()) {
            const auto identity=stage->DefinePrim(SdfPath("/Rig/Mechanisms/identityParent"),TfToken("Scope"));
            auto matrix=identity.CreateAttribute(TfToken("outputs:matrix"),SdfValueTypeNames->Matrix4d);
            matrix.Set(GfMatrix4d(1)); identityParent=matrix.GetPath();
        }
        return identityParent;
    }
    void AbsolutePoseParent(const UsdPrim &prim) {
        // A connected parent space is an explicit inheritance boundary in
        // usdRig, including when its value is identity. A literal identity
        // leaves namespace propagation enabled and duplicates pose refresh.
        prim.GetAttribute(TfToken("parent:space")).SetConnections({IdentitySource()});
    }
    Node &Find(const std::string &id) {
        auto it=nodes.find(id);
        if(it==nodes.end()) throw std::runtime_error("unresolved rig node: "+id);
        return it->second;
    }
    rigExec::RigExecMoverChain Chain(const std::string &name,const SdfPath &target={}) {
        // Evaluation walks reverse composed order. The authored reorder is
        // the reverse of source application order, including constraint
        // stacks; Run reverses this list once.
        operationOrder.emplace_back(name);
        return rig.NewMoverChain(name,target);
    }
    // Enabled constraints with positive influence per owner, in source order.
    const std::map<std::string,std::vector<size_t>> &ActiveStacks() const {
        if(!activeStacksBuilt) {
            std::map<std::string,std::vector<size_t>> stacks;
            const auto &items=Array(Field(scene,"constraints"));
            for(size_t i=0;i<items.size();++i) {
                const auto owner=Str(items[i],"owner");
                if(Flag(items[i],"enabled",true) && Number(Field(items[i],"influence"))>0 && !pruned.count(owner)) stacks[owner].push_back(i);
            }
            activeStacks=std::move(stacks); activeStacksBuilt=true;
        }
        return activeStacks;
    }
    bool HasActiveConstraint(const Node &node) const { return ActiveStacks().count(node.id)!=0; }
    bool EditableBoneControl(const Node &bone) const {
        if(bone.kind!="joint" || !Has(bone.data,"armature") ||
           Flag(bone.data,"connected",false) || !Flag(bone.data,"local_location",true)) return false;
        if(!bone.parent.empty()) {
            const auto &parent=nodes.at(bone.parent);
            if(parent.kind=="joint" &&
               (!Flag(bone.data,"inherit_rotation",true) ||
                (Has(bone.data,"inherit_scale") && Str(bone.data,"inherit_scale")!="FULL"))) return false;
        }
        return true;
    }
    // An eligible bone with a rigid rest needs no neutral frame provider: its
    // Control switches into the bone's Dag parent and holds the raw rest over
    // that parent's default space. A non-rigid rest keeps the neutral Blender
    // frame, the only path validated for scale, shear and reflection.
    bool NativeControlSpace(const Node &bone) const { return EditableBoneControl(bone) && Rigid(bone.rest); }
    // Defining siblings one at a time recomposes their parent's children on
    // every call. Graph creates its typed prim specs inside one change block,
    // in the order they are defined below; Define then reuses the composed prims.
    void Spec(const SdfPath &path,const char *type) {
        const auto layer=stage->GetRootLayer();
        if(!layer->GetPrimAtPath(path)) SdfPrimSpec::New(layer->GetPrimAtPath(path.GetParentPath()),path.GetName(),SdfSpecifierDef,type);
    }
    void Place(Node &n,size_t depth=0) {
        if(depth>512) throw std::runtime_error("rig hierarchy exceeds 512 levels");
        if(n.state==2) return;
        if(n.state==1) throw std::runtime_error("rig hierarchy cycle: "+n.id);
        n.state=1;
        GfMatrix4d bindLocal=n.rest;
        if(Flag(n.data,"pose_is_object_basis",false)) bindLocal=PoseMatrix(Array(Field(n.data,"pose")))*bindLocal;
        SdfPath parent("/Rig/Dag");
        n.restFrame=n.rest; n.restFrame.Orthonormalize(false);
        if(!n.parent.empty()) { auto &p=Find(n.parent); Place(p,depth+1); parent=p.path; n.world=bindLocal*p.world; n.restFrame*=p.restFrame; }
        else n.world=bindLocal;
        n.path=parent.AppendChild(TfToken(n.name));
        n.controlPath=n.path;
        Spec(n.path,n.kind=="joint" ? "RigExecJoint" : "RigExecControl");
        n.state=2;
    }
    void Build(Node &n) {
        const auto &pose=Array(Field(n.data,"pose"));
        auto schema=rigExec::RigExecSchemaPrim::Define(stage,n.path,TfToken(n.kind=="joint" ? "RigExecJoint" : "RigExecControl"));
        schema.ApplyAPI(TfToken("NodeGraphNodeAPI"));
        schema.ApplyAPI(TfToken("RigExecControlAPI"));
        Set(schema,TfToken("rest:space"),VtValue(n.rest));
        auto prim=schema.GetPrim(); prim.SetDisplayName(Str(n.data,"name"));
        // Purpose stays authored: UsdGeom only inherits an authored opinion.
        UsdGeomImageable(prim).CreatePurposeAttr().Set(UsdGeomTokens->guide);
        prim.SetCustomDataByKey(TfToken("blender:id"),VtValue(n.id));
        if(n.kind=="joint") prim.SetCustomDataByKey(TfToken("blender:deform"),VtValue(Flag(n.data,"deform",false)));
        if(!Rigid(n.rest)) Warn("rest scale/shear/reflection retained; RigExec orthonormalizes rest frames: "+n.id);
        if(pose.size()!=9) throw std::runtime_error("pose must contain 9 TRS channels");
        // Graph connects every armature bone avar to its Control. The saved
        // pose stays on the bone for readers that do not follow connections.
        const bool relayed=n.kind=="joint" && Has(n.data,"armature");
        const char *channels[]={"tx","ty","tz","rx","ry","rz","sx","sy","sz"};
        for(int i=0;i<9;++i) {
            double value=Number(pose[i]);
            if(i>=6 && std::abs(value)<1e-4) Warn("near-zero pose scale is clamped by RigExec: "+n.id);
            Set(schema,TfToken(std::string("avars:")+channels[i]),VtValue(value));
        }
        Set(schema,TfToken("avars:rotationOrder"),VtValue(TfToken("XYZ")));
        const bool visible=Flag(n.data,"visible",true);
        if(n.kind=="control") Set(schema,TfToken("guide:displayOpacity"),VtValue(
            visible && Has(n.data,"object_type") && Str(n.data,"object_type")=="EMPTY" ? 0.8f : 0.0f));
        else Set(schema,TfToken("guide:radius"),VtValue(
            !relayed && visible && !Has(n.data,"guide_points") ? Num(n.data,"length",1)*0.04 : 0.0));
        // USD visibility propagates down namespace ancestry, unlike Blender
        // bone collection visibility. Hide each guide, not its pose provider.
    }
    // Opt-in and not 1:1. An armature is observed when a bone of it is visible
    // or is named anywhere in the snapshot outside that armature's own bones
    // and their constraints: picker targets, skin influences, constraint
    // sources/poles/targets, guide sources, hooks and parents of kept nodes.
    // References are found by value, so a new snapshot field cannot be missed.
    // A reference from another armature counts only if that one is kept.
    void PruneUnobserved() {
        std::map<std::string,std::string> armatures;
        std::map<std::string,size_t> bones;
        for(const auto &entry:nodes) if(entry.second.kind=="joint" && Has(entry.second.data,"armature")) {
            armatures[entry.first]=Str(entry.second.data,"armature"); ++bones[armatures[entry.first]];
        }
        std::map<std::string,std::set<std::string>> observes;
        std::function<void(const JsValue &,const std::string &)> scan=[&](const JsValue &value,const std::string &from) {
            if(value.IsString()) {
                const auto bone=armatures.find(value.GetString());
                if(bone!=armatures.end() && bone->second!=from) observes[from].insert(bone->second);
            } else if(value.IsArray()) { for(const auto &item:value.GetJsArray()) scan(item,from); }
            else if(value.IsObject()) { for(const auto &item:value.GetJsObject()) scan(item.second,from); }
        };
        auto owner=[&](const std::string &id) { const auto bone=armatures.find(id); return bone==armatures.end() ? std::string() : bone->second; };
        for(const auto &entry:Object(scene)) {
            if(entry.first=="nodes") for(const auto &node:Array(entry.second)) {
                const auto from=owner(Str(node,"id")); scan(node,from);
                if(!from.empty() && Flag(node,"visible",true)) observes[""].insert(from);
            }
            else if(entry.first=="constraints") for(const auto &c:Array(entry.second)) scan(c,owner(Str(c,"owner")));
            else scan(entry.second,"");
        }
        std::set<std::string> kept{""};
        std::vector<std::string> todo{""};
        while(!todo.empty()) {
            const auto from=todo.back(); todo.pop_back();
            for(const auto &armature:observes[from]) if(kept.insert(armature).second) todo.push_back(armature);
        }
        for(const auto &entry:armatures) if(!kept.count(entry.second)) pruned.insert(entry.first);
        for(const auto &entry:bones) if(!kept.count(entry.first))
            Warn("unobserved armature omitted (pruneUnobserved): "+entry.first+" ("+std::to_string(entry.second)+" bones)");
        for(const auto &id:pruned) nodes.erase(id);
        activeStacksBuilt=false;
    }
    void Graph() {
        stage->DefinePrim(SdfPath("/Rig/Dag"),TfToken("Scope"));
        stage->DefinePrim(SdfPath("/Rig/Controls"),TfToken("Scope"));
        for(const auto &data:Array(Field(scene,"nodes"))) {
            Node n; n.data=data; n.id=Str(data,"id"); n.parent=Str(data,"parent");
            n.kind=Str(data,"kind"); n.rest=Matrix(Field(data,"rest"));
            if(n.id.empty() || (n.kind!="control" && n.kind!="joint")) throw std::runtime_error("invalid node identity/kind");
            if(n.kind=="joint" && Num(data,"length",1)<=0) throw std::runtime_error("non-positive bone length");
            if(!nodes.emplace(n.id,std::move(n)).second) throw std::runtime_error("duplicate node ID");
        }
        std::map<std::string,std::string> labels;
        for(const auto &entry:nodes) labels[entry.first]=Str(entry.second.data,"name");
        const auto names=PrimNames(labels);
        for(auto &entry:nodes) entry.second.name=names.at(entry.first);
        // Named first: the surviving prim paths equal the unpruned ones.
        if(prune) PruneUnobserved();
        const auto armatureBone=[](const Node &n) { return n.kind=="joint" && Has(n.data,"armature"); };
        if(std::any_of(nodes.begin(),nodes.end(),[&](const auto &entry) { return armatureBone(entry.second) && !NativeControlSpace(entry.second); }))
            stage->DefinePrim(SdfPath("/Rig/ControlSpaces"),TfToken("Scope"));
        if(std::any_of(nodes.begin(),nodes.end(),[&](const auto &entry) { return armatureBone(entry.second) && !EditableBoneControl(entry.second); }))
            stage->DefinePrim(SdfPath("/Rig/TranslationSpaces"),TfToken("Scope"));
        {
            SdfChangeBlock block;
            const SdfPath mechanisms("/Rig/Mechanisms"),controls("/Rig/Controls"),spaces("/Rig/ControlSpaces"),translations("/Rig/TranslationSpaces");
            for(auto &entry:nodes) Place(entry.second);
            for(const auto &entry:nodes) {
                const auto &bone=entry.second;
                if(!armatureBone(bone)) continue;
                const TfToken name(bone.name),neutral("controlFrame_"+bone.name);
                Spec(mechanisms,"");
                Spec(mechanisms.AppendChild(TfToken("bone_"+bone.name)),"RigExecBoneFrame");
                Spec(mechanisms.AppendChild(TfToken("identityParent")),"Scope");
                if(!NativeControlSpace(bone)) {
                    Spec(spaces.AppendChild(name),"RigExecControl");
                    Spec(mechanisms.AppendChild(neutral),"RigExecBoneFrame");
                }
                Spec(controls.AppendChild(name),"Scope");
                Spec(controls.AppendChild(name).AppendChild(TfToken("Control")),"RigExecControl");
                if(EditableBoneControl(bone)) Spec(mechanisms.AppendChild(TfToken("controlSpace_"+bone.name)),"RigExecSpaceSwitch");
                else {
                    Spec(translations.AppendChild(name),"RigExecControl");
                    Spec(mechanisms.AppendChild(neutral).AppendChild(TfToken("Translation")),"RigExecBoneFrame");
                }
            }
        }
        for(auto &entry:nodes) Build(entry.second);
        for(auto &entry:nodes) {
            auto &bone=entry.second;
            if(!armatureBone(bone)) continue;
            const auto mode=Has(bone.data,"inherit_scale") ? Str(bone.data,"inherit_scale") : "FULL";
            static const std::set<std::string> modes{"FULL","NONE","AVERAGE","ALIGNED","FIX_SHEAR","NONE_LEGACY"};
            if(!modes.count(mode)) throw std::runtime_error("unknown bone inheritance mode: "+mode);
            auto &object=Find(Str(bone.data,"armature"));
            auto &parent=Find(bone.parent);
            SdfPathVector dependencies={object.path};
            if(parent.kind=="joint") dependencies.push_back(parent.path);
            // The live bone expression and its neutral editor frames differ
            // only in connection state, channel inputs and published basis.
            auto boneFrame=[&](const SdfPath &path,bool connected,const char *spaceKind) {
                auto frame=rigExec::RigExecSchemaPrim::Define(stage,path,TfToken("RigExecBoneFrame"));
                Set(frame,TfToken("inputs:local"),VtValue(bone.rest));
                Set(frame,TfToken("inputs:inheritScale"),VtValue(TfToken(mode)));
                Set(frame,TfToken("inputs:inheritRotation"),VtValue(Flag(bone.data,"inherit_rotation",true)));
                Set(frame,TfToken("inputs:localLocation"),VtValue(Flag(bone.data,"local_location",true)));
                Set(frame,TfToken("inputs:connected"),VtValue(connected));
                if(parent.kind=="joint") {
                    Set(frame,TfToken("inputs:hasParent"),VtValue(true));
                    Set(frame,TfToken("inputs:parentRest"),VtValue(parent.world*object.world.GetInverse()));
                    frame.SetRelationship(TfToken("rigExec:parent"),{parent.path});
                }
                frame.SetRelationship(TfToken("rigExec:sourceObject"),{object.path});
                frame.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
                if(spaceKind) Set(frame,TfToken("inputs:spaceKind"),VtValue(TfToken(spaceKind)));
                return frame;
            };
            const SdfPath controlParent=SdfPath("/Rig/Controls").AppendChild(TfToken(bone.name));
            const SdfPath controlPath=controlParent.AppendChild(TfToken("Control"));
            auto joint=stage->GetPrimAtPath(bone.path);
            auto mechanism=boneFrame(SdfPath("/Rig/Mechanisms").AppendChild(TfToken("bone_"+bone.name)),Flag(bone.data,"connected",false),nullptr);
            // The expression reads the animator channels at their source; the
            // joint avars below stay connected for consumers of the bone prim.
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"})
                mechanism.GetPrim().GetAttribute(TfToken(std::string("inputs:")+channel)).SetConnections({controlPath.AppendProperty(TfToken(std::string("avars:")+channel))});
            // This expression already returns an absolute world frame and
            // explicitly reads its live parent. Namespace pose propagation
            // must stop here; dependency refresh computes inheritance once.
            AbsolutePoseParent(joint);
            joint.GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            const bool splitChannels=!EditableBoneControl(bone),nativeSpace=NativeControlSpace(bone);
            const SdfPath framePath=SdfPath("/Rig/ControlSpaces").AppendChild(TfToken(bone.name));
            const SdfPath neutralPath=SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlFrame_"+bone.name));
            const SdfPath switchPath=SdfPath("/Rig/Mechanisms").AppendChild(TfToken("controlSpace_"+bone.name));
            if(!nativeSpace) {
                auto frame=rigExec::RigExecSchemaPrim::Define(stage,framePath,TfToken("RigExecControl"));
                Set(frame,TfToken("rest:space"),VtValue(GfMatrix4d(1)));
                Set(frame,TfToken("default:space"),VtValue(GfMatrix4d(1)));
                Set(frame,TfToken("guide:displayOpacity"),VtValue(0.0f));
                // Split channels publish independent bases: Blender inherits
                // location and rotation/scale through different parent transforms.
                auto neutral=boneFrame(neutralPath,false,splitChannels ? "rotation" : nullptr);
                frame.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({neutral.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            }
            stage->DefinePrim(controlParent,TfToken("Scope"));
            auto control=rigExec::RigExecSchemaPrim::Define(stage,controlPath,TfToken("RigExecControl"));
            control.ApplyAPI(TfToken("RigExecControlAPI"));
            control.ApplyAPI(TfToken("NodeGraphNodeAPI"));
            Set(control,TfToken("rest:space"),VtValue(GfMatrix4d(1)));
            Set(control,TfToken("avars:rotationOrder"),VtValue(TfToken("XYZ")));
            auto prim=control.GetPrim();
            // The switch composes avars * default * inverse(parent default) *
            // parent posed. The parent default is its orthonormalized rest
            // chain, so this literal cancels it and leaves the raw rest over
            // the posed parent: the neutral Blender frame, to rounding. The
            // joint's own default:space would orthonormalize the rest too.
            // An authored default refuses stock Pivot editing; an identity
            // one resolves like no opinion, so it is connected instead.
            const GfMatrix4d nativeDefault=bone.rest*parent.restFrame;
            if(nativeSpace && nativeDefault!=GfMatrix4d(1)) Set(control,TfToken("default:space"),VtValue(nativeDefault));
            else prim.GetAttribute(TfToken("default:space")).SetConnections({nativeSpace ? IdentitySource() : framePath.AppendProperty(TfToken("default:space"))});
            prim.SetDisplayName(Str(bone.data,"name"));
            prim.SetCustomDataByKey(TfToken("blender:sourceId"),VtValue(bone.id));
            UsdGeomImageable(prim).CreatePurposeAttr().Set(UsdGeomTokens->guide);
            if(!splitChannels) {
            auto spaceSwitch=rigExec::RigExecSchemaPrim::Define(stage,switchPath,TfToken("RigExecSpaceSwitch"));
            spaceSwitch.SetRelationship(TfToken("rigExec:target"),{controlPath});
            // The switch replaces the Control's parent space: the bone's own
            // Dag parent natively, else the neutral Blender frame.
            spaceSwitch.SetRelationship(TfToken("rigExec:sources"),{nativeSpace ? parent.path : framePath});
            Set(spaceSwitch,TfToken("inputs:activeSpace"),VtValue(0.0));
            }
            const bool customShape=Has(bone.data,"guide_points") && !Array(Field(bone.data,"guide_points")).empty();
            const bool ownShape=!Has(bone.data,"guide_source") || Str(bone.data,"guide_source").empty() || Str(bone.data,"guide_source")==bone.id;
            // Guides authors the opacity and unit scale of a custom shape.
            if(!customShape) {
                Set(control,TfToken("guide:displayOpacity"),VtValue(Flag(bone.data,"visible",true) ? 0.8f : 0.0f));
                for(const char *axis:{"guide:scaleX","guide:scaleY","guide:scaleZ"})
                    Set(control,TfToken(axis),VtValue(Num(bone.data,"length",1)*0.04));
            }
            const auto &pose=Array(Field(bone.data,"pose"));
            const char *channels[]={"tx","ty","tz","rx","ry","rz","sx","sy","sz"};
            for(int i=0;i<9;++i) {
                const TfToken channel(std::string("avars:")+channels[i]);
                Set(control,channel,VtValue(Number(pose[i])));
                joint.GetAttribute(channel).SetConnections({controlPath.AppendProperty(channel)});
            }
            joint.CreateRelationship(TfToken("blender:channelControl"),true).SetTargets({controlPath});
            if(splitChannels) {
                // A sibling of the rotation basis, not its child: under the
                // static holder it needs no inheritance boundary and is not
                // revisited when the rotation basis changes.
                const auto translationPath=SdfPath("/Rig/TranslationSpaces").AppendChild(TfToken(bone.name));
                auto translation=rigExec::RigExecSchemaPrim::Define(stage,translationPath,TfToken("RigExecControl"));
                Set(translation,TfToken("guide:displayOpacity"),VtValue(0.0f));
                auto translationExpr=boneFrame(neutralPath.AppendChild(TfToken("Translation")),false,"translation");
                translation.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({translationExpr.GetPath().AppendProperty(TfToken("outputs:matrix"))});
                control.SetRelationship(TfToken("rigExec:channelSpaces"),{framePath,translationPath});
                Set(control,TfToken("rigExec:translationEnabled"),VtValue(!Flag(bone.data,"connected",false)));
                // The original bone expression consumes these same nine channels.
                // Its pre-constraint pose is authoritative for the editor too.
                prim.GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            }
            bone.controlPath=controlPath;
            if(HasActiveConstraint(bone) && customShape && ownShape)
                Warn("native control guide displays pre-constraint edit frame: "+bone.id);
        }
    }
    double GuideWidth(const Node &node) {
        const double authored=Num(node.data,"guide_wire_width",0);
        if(authored>0) return authored;
        // Older snapshots authored zero hairline widths. Reconstruct a modest
        // armature-sized diameter once per armature for native Hydra picking.
        const std::string armature=Has(node.data,"armature") ? Str(node.data,"armature") : node.id;
        auto found=guideExtents.find(armature);
        if(found==guideExtents.end()) {
            GfVec3d low(std::numeric_limits<double>::max()),high(-std::numeric_limits<double>::max());
            const auto object=nodes.find(armature);
            const GfMatrix4d inverse=object==nodes.end() ? GfMatrix4d(1) : object->second.world.GetInverse();
            bool any=false;
            for(const auto &entry:nodes) {
                const auto &bone=entry.second;
                if(bone.kind!="joint" || !Has(bone.data,"armature") || Str(bone.data,"armature")!=armature ||
                   !Flag(bone.data,"deform",true)) continue;
                const auto frame=bone.world*inverse;
                for(const auto &point:{frame.ExtractTranslation(),frame.Transform(GfVec3d(0,Num(bone.data,"length",1),0))})
                    for(int axis=0;axis<3;++axis) {low[axis]=std::min(low[axis],point[axis]);high[axis]=std::max(high[axis],point[axis]);}
                any=true;
            }
            const double extent=any ? std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2]}) : Num(node.data,"length",1);
            found=guideExtents.emplace(armature,std::max(extent,1e-6)).first;
        }
        return found->second*0.001*std::max(0.5,Num(node.data,"source_wire_width_pixels",1));
    }
    void Guides() {
        for(auto &entry:nodes) {
            auto &n=entry.second;
            if(!Has(n.data,"guide_points") || Array(Field(n.data,"guide_points")).empty())continue;
            const bool ownShape=!Has(n.data,"guide_source") || Str(n.data,"guide_source").empty() || Str(n.data,"guide_source")==n.id;
            if(n.controlPath!=n.path || (n.kind=="control" && !HasActiveConstraint(n) && ownShape)) {
                auto control=rigExec::RigExecSchemaPrim::Define(stage,n.controlPath,TfToken("RigExecControl"));
                VtVec3fArray points;VtIntArray counts;size_t total=0;
                for(const auto &p:Array(Field(n.data,"guide_points")))points.push_back(Point(p));
                for(const auto &v:Array(Field(n.data,"guide_counts"))) {
                    int count=Integer(v);if(count<2)throw std::runtime_error("invalid guide polyline count");
                    counts.push_back(count);total+=count;
                }
                if(total!=points.size())throw std::runtime_error("guide topology mismatch");
                if(!ownShape) control.SetRelationship(TfToken("guide:source"),{Find(Str(n.data,"guide_source")).path});
                Set(control,TfToken("guide:shape"),VtValue(TfToken("custom")));
                Set(control,TfToken("guide:points"),VtValue(points));
                Set(control,TfToken("guide:curveVertexCounts"),VtValue(counts));
                Set(control,TfToken("guide:wireWidth"),VtValue(GuideWidth(n)));
                Set(control,TfToken("guide:displayOpacity"),VtValue(Flag(n.data,"visible",true)?1.0f:0.0f));
                Set(control,TfToken("guide:scaleX"),VtValue(1.0));
                Set(control,TfToken("guide:scaleY"),VtValue(1.0));
                Set(control,TfToken("guide:scaleZ"),VtValue(1.0));
                if(Has(n.data,"guide_color"))Set(control,TfToken("guide:displayColor"),VtValue(Point(Field(n.data,"guide_color"))));
                continue;
            }
            if(!ownShape) Warn("guide follows a different source and is not the native editable control: "+n.id);
            auto frame=[&](rigExec::RigExecSchemaPrim &target,Node &sourceNode,const std::string &suffix) {
                auto expression=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("guide_"+n.name+suffix)),TfToken("RigExecCopyFrame"));
                expression.SetRelationship(TfToken("rigExec:source"),{sourceNode.path});
                expression.SetRelationship(TfToken("rigExec:poseInputs"),{sourceNode.path});
                AbsolutePoseParent(target.GetPrim());
                target.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({expression.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            };
            auto guide=rigExec::RigExecSchemaPrim::Define(stage,n.path.AppendChild(TfToken("Display")),TfToken("RigExecControl"));
            Node &shapeSource=Has(n.data,"guide_source") && !Str(n.data,"guide_source").empty() ? Find(Str(n.data,"guide_source")) : n;
            frame(guide,shapeSource,"_display");
            UsdGeomImageable(guide.GetPrim()).CreatePurposeAttr().Set(UsdGeomTokens->guide);
            VtVec3fArray points;VtIntArray counts;size_t total=0;
            for(const auto &p:Array(Field(n.data,"guide_points")))points.push_back(Point(p));
            for(const auto &v:Array(Field(n.data,"guide_counts"))) {
                int count=Integer(v);if(count<2)throw std::runtime_error("invalid guide polyline count");
                counts.push_back(count);total+=count;
            }
            if(total!=points.size())throw std::runtime_error("guide topology mismatch");
            Set(guide,TfToken("guide:shape"),VtValue(TfToken("custom")));
            Set(guide,TfToken("guide:points"),VtValue(points));
            Set(guide,TfToken("guide:curveVertexCounts"),VtValue(counts));
            Set(guide,TfToken("guide:wireWidth"),VtValue(GuideWidth(n)));
            Set(guide,TfToken("guide:displayOpacity"),VtValue(Flag(n.data,"visible",true)?1.0f:0.0f));
            if(Has(n.data,"guide_color"))Set(guide,TfToken("guide:displayColor"),VtValue(Point(Field(n.data,"guide_color"))));
        }
    }
    bool SplineIk(Node &owner,const JsValue &c,size_t index) {
        if(owner.kind!="joint" || !Has(owner.data,"armature") || Str(c,"source").empty() ||
           Number(Field(c,"influence"))!=1 || Num(c,"chain_count",0)<2 ||
           !Has(c,"y_scale_mode") || Str(c,"y_scale_mode")!="FIT_CURVE" ||
           !Has(c,"xz_scale_mode") || Str(c,"xz_scale_mode")!="NONE" ||
           !Flag(c,"use_even_divisions",false) || Flag(c,"use_chain_offset",false))return false;
        auto &curve=Find(Str(c,"source"));
        if(!Has(curve.data,"spline_hooks") || Array(Field(curve.data,"spline_hooks")).size()!=3 ||
           !Has(curve.data,"spline_radii"))return false;
        for(const auto &radius:Array(Field(curve.data,"spline_radii")))
            if(std::abs(Number(radius)-1)>1e-6)return false;
        std::vector<Node *> hooks,chain;
        for(const auto &id:Array(Field(curve.data,"spline_hooks"))) {
            auto &bone=Find(String(id));
            if(bone.kind!="joint" || Str(bone.data,"armature")!=Str(owner.data,"armature"))return false;
            hooks.push_back(&bone);
        }
        Node *bone=&owner;
        const int count=static_cast<int>(Num(c,"chain_count",0));
        for(int i=0;i<count;++i) {
            if(bone->kind!="joint" || Str(bone->data,"armature")!=Str(owner.data,"armature"))return false;
            chain.push_back(bone);
            if(i+1<count) {
                if(bone->parent.empty())return false;
                bone=&Find(bone->parent);
            }
        }
        std::reverse(chain.begin(),chain.end());
        const auto name="spline_"+std::to_string(index);
        const auto controlScope=SdfPath("/Rig/SplineControls").AppendChild(TfToken(name));
        const auto jointScope=SdfPath("/Rig/SplineJoints").AppendChild(TfToken(name));
        stage->DefinePrim(SdfPath("/Rig/SplineControls"),TfToken("Scope"));
        stage->DefinePrim(SdfPath("/Rig/SplineJoints"),TfToken("Scope"));
        stage->DefinePrim(controlScope,TfToken("Scope"));
        stage->DefinePrim(jointScope,TfToken("Scope"));
        SdfPathVector controls,joints;
        for(int i=0;i<3;++i) {
            auto proxy=rigExec::RigExecSchemaPrim::Define(stage,controlScope.AppendChild(TfToken("Hook"+std::to_string(i))),TfToken("RigExecControl"));
            Set(proxy,TfToken("rest:space"),VtValue(hooks[i]->world));
            Set(proxy,TfToken("guide:displayOpacity"),VtValue(0.0f));
            proxy.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({hooks[i]->path.AppendProperty(TfToken("posed:space"))});
            controls.push_back(proxy.GetPath());
        }
        std::vector<GfMatrix4d> virtualRests;
        for(int i=0;i<=count;++i) {
            auto rest=chain[std::min(i,count-1)]->world;
            if(i==count)rest.SetTranslateOnly(rest.Transform(GfVec3d(0,Num(owner.data,"length",1),0)));
            GfVec3d x(rest[1][0],rest[1][1],rest[1][2]);
            GfVec3d y(-rest[0][0],-rest[0][1],-rest[0][2]);
            for(int axis=0;axis<3;++axis) {rest[0][axis]=x[axis];rest[1][axis]=y[axis];}
            auto virtualJoint=rigExec::RigExecSchemaPrim::Define(stage,jointScope.AppendChild(TfToken("Joint"+std::to_string(i))),TfToken("RigExecJoint"));
            Set(virtualJoint,TfToken("rest:space"),VtValue(rest));
            Set(virtualJoint,TfToken("guide:radius"),VtValue(0.0));
            Set(virtualJoint,TfToken("guide:displayOpacity"),VtValue(0.0f));
            virtualRests.push_back(rest);joints.push_back(virtualJoint.GetPath());
        }
        auto solver=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Solvers").AppendChild(TfToken(name)),TfToken("RigExecSplineIk"));
        solver.ApplyAPI(TfToken("NodeGraphNodeAPI"));
        // The stock solver publishes a unit-radius blue guide sphere. Its
        // three hook controls are the animator-facing guides instead.
        UsdGeomImageable(solver.GetPrim()).CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
        Set(solver,TfToken("guide:radius"),VtValue(0.0));
        Set(solver,TfToken("guide:displayOpacity"),VtValue(0.0f));
        solver.SetRelationship(TfToken("rigExec:rootControl"),{controls[0]});
        solver.SetRelationship(TfToken("rigExec:midControl"),{controls[1]});
        solver.SetRelationship(TfToken("rigExec:endControl"),{controls[2]});
        solver.SetRelationship(TfToken("rigExec:joints"),joints);
        Set(solver,TfToken("inputs:preserveVolume"),VtValue(0.0));
        Set(solver,TfToken("inputs:midFollowWeight"),VtValue(0.35));
        Set(solver,TfToken("rigExec:restLength"),VtValue(TfToken("curve")));
        for(int i=0;i<count;++i) {
            auto mapped=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken(name+"_bone_"+std::to_string(i))),TfToken("RigExecMappedFrame"));
            Set(mapped,TfToken("inputs:targetRest"),VtValue(chain[i]->world));
            Set(mapped,TfToken("inputs:sourceRest"),VtValue(virtualRests[i]));
            mapped.SetRelationship(TfToken("rigExec:source"),{joints[i]});
            mapped.SetRelationship(TfToken("rigExec:poseInputs"),{joints[i]});
            stage->GetPrimAtPath(chain[i]->path).GetAttribute(TfToken("posed:space")).SetConnections({mapped.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        }
        Warn("Spline IK uses native three-control curve; Blender Bezier hook shape and twist may differ: "+owner.id);
        return true;
    }
    void Constraints() {
        const auto &items=Array(Field(scene,"constraints"));
        // Dependencies include ancestors of every source and owner. Reject
        // feedback explicitly, then apply each owner's stack in source order.
        for(const auto &c:items) {
            const auto owner=Str(c,"owner");
            if(!pruned.count(owner)) Find(owner);
            const double influence=Number(Field(c,"influence"));
            if(influence<0 || influence>1) throw std::runtime_error("constraint influence outside [0,1]");
        }
        auto stacks=ActiveStacks(); // Copied: visiting adds empty dependency-only stacks.
        std::map<std::string,int> state;
        size_t depth=0;
        std::function<void(const std::string &)> visit;
        visit=[&](const std::string &owner) {
            if(state[owner]==2) return;
            if(state[owner]==1) throw std::runtime_error("constraint dependency cycle: "+owner);
            if(++depth>512) throw std::runtime_error("constraint dependency depth exceeds 512");
            state[owner]=1;
            const auto &node=Find(owner);
            if(!node.parent.empty()) visit(node.parent);
            for(size_t index:stacks[owner]) {
                const auto &c=items[index];
                const auto src=Str(c,"source");
                if(!src.empty()) {
                    visit(src);
                    if(Str(c,"type")=="SPLINE_IK" && Has(Find(src).data,"spline_hooks"))
                        for(const auto &hook:Array(Field(Find(src).data,"spline_hooks")))visit(String(hook));
                }
                if(Has(c,"pole")) visit(Str(c,"pole"));
                if(Has(c,"custom_space") && !Str(c,"custom_space").empty())visit(Str(c,"custom_space"));
                if(Str(c,"type")=="ARMATURE" && Has(c,"targets"))
                    for(const auto &target:Array(Field(c,"targets")))
                        if(Number(Field(target,"weight"))>0 && !Str(target,"source").empty())visit(Str(target,"source"));
            }
            // Each constraint is validated once, in source order, stopping
            // at the first unsupported one unless a subsequence may be kept.
            const auto &stack=stacks[owner];
            auto &ownerNode=Find(owner);
            std::vector<size_t> subset;
            size_t checked=0;
            for(;checked<stack.size() && NativeConstraint(ownerNode,items[stack[checked]]);++checked) subset.push_back(stack[checked]);
            bool native=!stack.empty() && checked==stack.size();
            const bool hasIk=std::any_of(stack.begin(),stack.end(),
                [&](size_t index){return Str(items[index],"type")=="IK";});
            if(!native && !hasIk) {
                // Unimplemented Action/Transform/limit nodes must not erase
                // the supported Armature parenting in the same stack. Keep
                // the native subsequence in source order, and diagnose every
                // omitted node; this remains an incomplete conversion.
                for(size_t i=checked+1;i<stack.size();++i)
                    if(NativeConstraint(ownerNode,items[stack[i]]))subset.push_back(stack[i]);
                if(!subset.empty()) {
                    native=true;
                    for(size_t index:stack)if(std::find(subset.begin(),subset.end(),index)==subset.end())
                        Warn("unsupported constraint omitted from native stack: "+owner+"/"+Str(items[index],"name")+" ("+Str(items[index],"type")+", "+Str(items[index],"owner_space")+"/"+Str(items[index],"target_space")+")");
                }
            }
            if(native)NativeConstraintStack(ownerNode,subset,items);
            else for(size_t i=0;i<stack.size();++i) {
                    size_t index=stack[i]; ConvertConstraint(items[index],index,i==0);
                }
            state[owner]=2;
            --depth;
        };
        std::vector<std::string> owners;
        for(const auto &entry:stacks) owners.push_back(entry.first);
        for(const auto &owner:owners) visit(owner);
    }
    // Whether one constraint can join its owner's native frame stack.
    bool NativeConstraint(Node &owner,const JsValue &c) {
        const bool boneOwner=owner.kind=="joint" && Has(owner.data,"armature");
        const bool objectOwner=owner.kind=="control" && Has(owner.data,"object_type");
        if(!boneOwner && !objectOwner)return false;
        {
            auto type=Str(c,"type");
            if(!boneOwner && type!="ARMATURE")return false;
            static const std::set<std::string> mapSources{"LOCATION","ROTATION","SCALE"},
                mapMixes{"ADD","REPLACE"},
                autoOrders{"XYZ","QUATERNION","AXIS_ANGLE"},
                axisNames{"X","Y","Z"},
                rotationMixes{"REPLACE","BEFORE","AFTER"},
                transformMixes{"REPLACE","BEFORE_FULL","AFTER_FULL","BEFORE","AFTER","BEFORE_SPLIT","AFTER_SPLIT"},
                volumes{"VOLUME_XZX","VOLUME_X","VOLUME_Z","NO_VOLUME"},
                keepAxes{"PLANE_X","PLANE_Z","SWING_Y"},
                trackAxes{"TRACK_X","TRACK_Y","TRACK_Z","TRACK_NEGATIVE_X","TRACK_NEGATIVE_Y","TRACK_NEGATIVE_Z"};
            static const std::set<std::string> types{"COPY_LOCATION","COPY_ROTATION","COPY_SCALE","COPY_TRANSFORMS","ARMATURE","STRETCH_TO","DAMPED_TRACK","LIMIT_ROTATION","TRANSFORM"};
            if(!types.count(type)) return false;
            bool copy=type.find("COPY_")==0;
            for(const char *space:{"owner_space","target_space"}) {
                auto value=Str(c,space);
                if(value!="WORLD" && !((copy || type=="TRANSFORM") && (value=="POSE" || value=="LOCAL" || value=="CUSTOM" ||
                    (copy && std::string(space)=="target_space" && value=="LOCAL_OWNER_ORIENT"))))return false;
                if(value=="CUSTOM" && (!Has(c,"custom_space") || Flag(c,"custom_space_unsupported",false)))return false;
            }
            if(type=="LIMIT_ROTATION") {
                if(!Has(c,"use_limit_x") || !Has(c,"use_limit_y") || !Has(c,"use_limit_z") ||
                   Flag(c,"use_limit_x",false) || Flag(c,"use_limit_y",false) || Flag(c,"use_limit_z",false))return false;
                return true; // With no angle limits this constraint only removes shear.
            }
            if(type=="ARMATURE") {
                if(Flag(c,"use_bone_envelopes",false) || !Has(c,"targets"))return false;
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));
                    if(weight<0)throw std::runtime_error("negative armature target weight");
                    if(weight==0)continue;
                    if(Str(target,"source").empty())return false;
                    auto &bone=Find(Str(target,"source"));
                    if(bone.kind!="joint" || !Has(bone.data,"armature"))return false;
                }
                return true;
            }
            if(Str(c,"source").empty())return false;
            auto &src=Find(Str(c,"source"));
            if(type=="TRANSFORM") {
                if(!Has(c,"map_from") || !Has(c,"map_to") || Str(c,"map_to")!="LOCATION" ||
                    !mapSources.count(Str(c,"map_from")) ||
                    !Has(c,"mix_mode") || !mapMixes.count(Str(c,"mix_mode")))return false;
                if(Str(c,"map_from")=="ROTATION") {
                    if(!Has(c,"from_rotation_mode") || (Str(c,"from_rotation_mode")!="XYZ" &&
                        (Str(c,"from_rotation_mode")!="AUTO" || (Has(owner.data,"rotation_mode") &&
                         !autoOrders.count(Str(owner.data,"rotation_mode"))))))return false;
                }
                const std::string suffix=Str(c,"map_from")=="ROTATION"?"_rot":Str(c,"map_from")=="SCALE"?"_scale":"";
                for(char axis:std::string("xyz")) {
                    const std::string a(1,axis);
                    if(!Has(c,"map_to_"+a+"_from") || !axisNames.count(Str(c,"map_to_"+a+"_from")))return false;
                    for(const auto &field:{"from_min_"+a+suffix,"from_max_"+a+suffix,"to_min_"+a,"to_max_"+a})
                        if(!Has(c,field))return false;
                    if(Number(Field(c,"from_min_"+a+suffix))>Number(Field(c,"from_max_"+a+suffix)))return false;
                }
            }
            if(Str(c,"target_space")!="WORLD" && Str(c,"target_space")!="CUSTOM" && (src.kind!="joint" || !Has(src.data,"armature")))return false;
            if(type=="COPY_ROTATION") {
                int count=Flag(c,"use_x",true)+Flag(c,"use_y",true)+Flag(c,"use_z",true);
                if((count!=0 && count!=3) || Flag(c,"invert_x",false) || Flag(c,"invert_y",false) || Flag(c,"invert_z",false) ||
                   (Has(c,"mix_mode") && !rotationMixes.count(Str(c,"mix_mode"))))return false;
            }
            if(type=="COPY_SCALE" && Num(c,"power",1)<0)throw std::runtime_error("negative Copy Scale power");
            if(Flag(c,"use_bbone_shape",false) && Num(src.data,"bbone_segments",1)>1 && Num(c,"head_tail",0)!=0) return false;
            if(type=="COPY_TRANSFORMS" && (Num(c,"head_tail",0)!=0 ||
                (Has(c,"mix_mode") && !transformMixes.count(Str(c,"mix_mode"))))) return false;
            if(type=="STRETCH_TO") {
                if(Num(c,"rest_length",0)<=0 || !Has(c,"volume") || !Has(c,"keep_axis"))return false;
                if(!volumes.count(Str(c,"volume")) ||
                   !keepAxes.count(Str(c,"keep_axis"))) throw std::runtime_error("invalid Stretch To mode");
                if(Num(c,"bulge",1)<0 || Num(c,"bulge_min",0)<0 || Num(c,"bulge_min",0)>1 || Num(c,"bulge_max",1)<1 ||
                   Num(c,"bulge_smooth",0)<0 || Num(c,"bulge_smooth",0)>1) throw std::runtime_error("invalid Stretch To volume settings");
            }
            if(type=="DAMPED_TRACK" && (!Has(c,"track_axis") || !trackAxes.count(Str(c,"track_axis"))))
                throw std::runtime_error("invalid Damped Track axis");
        }
        return true;
    }
    // Authors a stack whose every member passed NativeConstraint.
    void NativeConstraintStack(Node &owner,const std::vector<size_t> &stack,const JsArray &items) {
        const bool boneOwner=owner.kind=="joint" && Has(owner.data,"armature");
        const bool objectOwner=owner.kind=="control" && Has(owner.data,"object_type");
        auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
        SdfPathVector inputs;posed.GetConnections(&inputs);
        if(objectOwner && inputs.empty()) {
            // Object Armature constraints consume the owner's complete live
            // world frame. The generic parent expression supplies that frame
            // without applying any source map before the constraint stack.
            auto base=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("objectBase_"+owner.name)),TfToken("RigExecArmatureParent"));
            Set(base,TfToken("inputs:local"),VtValue(owner.rest));
            if(!owner.parent.empty()) {
                const auto parent=Find(owner.parent).path;
                base.SetRelationship(TfToken("rigExec:parent"),{parent});
                base.SetRelationship(TfToken("rigExec:poseInputs"),{parent});
            }
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"})
                base.GetPrim().GetAttribute(TfToken(std::string("inputs:")+channel)).SetConnections({owner.controlPath.AppendProperty(TfToken(std::string("avars:")+channel))});
            inputs.push_back(base.GetPath().AppendProperty(TfToken("outputs:matrix")));
            AbsolutePoseParent(stage->GetPrimAtPath(owner.path));
        }
        if(inputs.size()!=1)throw std::runtime_error("native constraint stack requires one incoming frame");
        auto base=inputs[0],previous=base;
        // The incoming expression reads the owner's live parent even when
        // the constraint itself only names target frames. Declare that
        // closure, and earlier stack inputs, for usdRig's pose refresh.
        SdfPathVector stackDependencies;
        if(boneOwner)stackDependencies.push_back(Find(Str(owner.data,"armature")).path);
        if(!owner.parent.empty() && (objectOwner || Find(owner.parent).kind=="joint"))stackDependencies.push_back(Find(owner.parent).path);
        for(size_t index:stack) {
            const auto &c=items[index];auto type=Str(c,"type");
            Node *src=Str(c,"source").empty()?nullptr:&Find(Str(c,"source"));
            auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("stack_"+std::to_string(index))),TfToken("RigExecConstraintFrame"));
            op.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections({previous});
            Set(op,TfToken("inputs:operation"),VtValue(TfToken(type)));
            Set(op,TfToken("inputs:influence"),VtValue(Number(Field(c,"influence"))));
            Set(op,TfToken("inputs:ownerSpace"),VtValue(TfToken(Str(c,"owner_space"))));
            Set(op,TfToken("inputs:targetSpace"),VtValue(TfToken(Str(c,"target_space"))));
            SdfPathVector dependencies=stackDependencies;
            if(src) {op.SetRelationship(TfToken("rigExec:source"),{src->path});dependencies.push_back(src->path);}
            if(Has(c,"custom_space") && !Str(c,"custom_space").empty()) {
                auto path=Find(Str(c,"custom_space")).path;
                op.SetRelationship(TfToken("rigExec:customSpace"),{path});dependencies.push_back(path);
            }
            auto localInputs=[&](Node &bone,const std::string &role) {
                auto &object=Find(Str(bone.data,"armature"));
                Set(op,TfToken("inputs:"+role+"Local"),VtValue(bone.rest));
                Set(op,TfToken("inputs:"+role+"Rest"),VtValue(bone.world*object.world.GetInverse()));
                Set(op,TfToken("inputs:"+role+"InheritScale"),VtValue(TfToken(Has(bone.data,"inherit_scale")?Str(bone.data,"inherit_scale"):"FULL")));
                Set(op,TfToken("inputs:"+role+"InheritRotation"),VtValue(Flag(bone.data,"inherit_rotation",true)));
                Set(op,TfToken("inputs:"+role+"LocalLocation"),VtValue(Flag(bone.data,"local_location",true)));
                auto &parent=Find(bone.parent);
                if(parent.kind=="joint") {
                    Set(op,TfToken("inputs:"+role+"HasParent"),VtValue(true));
                    Set(op,TfToken("inputs:"+role+"ParentRest"),VtValue(parent.world*object.world.GetInverse()));
                    op.SetRelationship(TfToken("rigExec:"+role+"Parent"),{parent.path});dependencies.push_back(parent.path);
                }
            };
            if(Str(c,"owner_space")!="WORLD" && Str(c,"owner_space")!="CUSTOM") {
                auto &object=Find(Str(owner.data,"armature"));
                op.SetRelationship(TfToken("rigExec:ownerObject"),{object.path});dependencies.push_back(object.path);
                if(Str(c,"owner_space")=="LOCAL")localInputs(owner,"owner");
            }
            if(Str(c,"target_space")!="WORLD" && Str(c,"target_space")!="CUSTOM") {
                auto &object=Find(Str(src->data,"armature"));
                op.SetRelationship(TfToken("rigExec:sourceObject"),{object.path});dependencies.push_back(object.path);
                if(Str(c,"target_space")!="POSE")localInputs(*src,"source");
                if(Str(c,"target_space")=="LOCAL_OWNER_ORIENT") {
                    auto &ownerObject=Find(Str(owner.data,"armature"));
                    Set(op,TfToken("inputs:ownerRest"),VtValue(owner.world*ownerObject.world.GetInverse()));
                }
            }
            if(src && src->kind=="joint")Set(op,TfToken("inputs:targetOffset"),VtValue(GfVec3d(0,Num(src->data,"length",1)*Num(c,"head_tail",0),0)));
            if(type=="ARMATURE") {
                Set(op,TfToken("inputs:operation"),VtValue(TfToken("ARMATURE_BLEND")));
                VtMatrix4dArray binds;VtDoubleArray weights;VtIntArray targetIndices,objectIndices;
                SdfPathVector targets,objects;
                auto add=[](SdfPathVector &paths,const SdfPath &path) {
                    auto it=std::find(paths.begin(),paths.end(),path);
                    if(it!=paths.end())return int(it-paths.begin());
                    paths.push_back(path);return int(paths.size()-1);
                };
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));if(weight==0)continue;
                    auto &bone=Find(Str(target,"source"));auto &object=Find(Str(bone.data,"armature"));
                    if(Num(bone.data,"bbone_segments",1)!=1)Warn("Armature B-Bone target uses its whole-bone frame; segment binding is not translated: "+bone.id);
                    binds.push_back((bone.world*object.world.GetInverse()).GetInverse());weights.push_back(weight);
                    targetIndices.push_back(add(targets,bone.path));objectIndices.push_back(add(objects,object.path));
                    add(dependencies,bone.path);add(dependencies,object.path);
                }
                Set(op,TfToken("inputs:targetBinds"),VtValue(binds));
                Set(op,TfToken("inputs:targetWeights"),VtValue(weights));
                Set(op,TfToken("inputs:targetIndices"),VtValue(targetIndices));
                Set(op,TfToken("inputs:objectIndices"),VtValue(objectIndices));
                Set(op,TfToken("inputs:dualQuaternion"),VtValue(Flag(c,"use_deform_preserve_volume",false)));
                // Objects always bind at the incoming origin; the optional
                // rest-head pivot in the source contract applies only to bones.
                Set(op,TfToken("inputs:currentPivot"),VtValue(objectOwner || Flag(c,"use_current_location",false)));
                if(boneOwner) {
                    auto &object=Find(Str(owner.data,"armature"));
                    Set(op,TfToken("inputs:pivot"),VtValue((owner.world*object.world.GetInverse()).ExtractTranslation()));
                    op.SetRelationship(TfToken("rigExec:ownerObject"),{object.path});add(dependencies,object.path);
                }
                op.SetRelationship(TfToken("rigExec:targets"),targets);op.SetRelationship(TfToken("rigExec:targetObjects"),objects);
            }
            if(type=="COPY_LOCATION" || type=="COPY_SCALE" || type=="COPY_ROTATION") {
                int axes=0,invert=0;const char *axisNames[]={"x","y","z"};
                for(int i=0;i<3;++i) {if(Flag(c,std::string("use_")+axisNames[i],true))axes|=1<<i;if(Flag(c,std::string("invert_")+axisNames[i],false))invert|=1<<i;}
                Set(op,TfToken("inputs:axisMask"),VtValue(axes));Set(op,TfToken("inputs:invertMask"),VtValue(invert));
                Set(op,TfToken("inputs:offset"),VtValue(Flag(c,"use_offset",false)));
            }
            if(type=="COPY_TRANSFORMS") {
                Set(op,TfToken("inputs:rotationMix"),VtValue(TfToken(Has(c,"mix_mode")?Str(c,"mix_mode"):"REPLACE")));
                Set(op,TfToken("inputs:removeTargetShear"),VtValue(Flag(c,"remove_target_shear",false)));
            }
            if(type=="TRANSFORM") {
                Set(op,TfToken("inputs:operation"),VtValue(TfToken("TRANSFORM_LOCATION")));
                Set(op,TfToken("inputs:mapFrom"),VtValue(TfToken(Str(c,"map_from"))));
                const std::string suffix=Str(c,"map_from")=="ROTATION"?"_rot":Str(c,"map_from")=="SCALE"?"_scale":"";
                GfVec3i axes;
                for(int i=0;i<3;++i)axes[i]=int(std::string("XYZ").find(Str(c,"map_to_"+std::string(1,"xyz"[i])+"_from")));
                Set(op,TfToken("inputs:mapAxes"),VtValue(axes));
                for(const auto &entry:std::vector<std::pair<std::string,std::string>>{{"from_min","mapFromMin"},{"from_max","mapFromMax"},{"to_min","mapToMin"},{"to_max","mapToMax"}}) {
                    GfVec3d values;
                    for(int i=0;i<3;++i)values[i]=Number(Field(c,entry.first+"_"+"xyz"[i]+(entry.first.find("from_")==0?suffix:"")));
                    Set(op,TfToken("inputs:"+entry.second),VtValue(values));
                }
                Set(op,TfToken("inputs:mapExtrapolate"),VtValue(Flag(c,"use_motion_extrapolate",false)));
                Set(op,TfToken("inputs:mapMix"),VtValue(TfToken(Str(c,"mix_mode"))));
            }
            if(type=="COPY_ROTATION")Set(op,TfToken("inputs:rotationMix"),VtValue(TfToken(Has(c,"mix_mode")?Str(c,"mix_mode"):"REPLACE")));
            if(type=="COPY_SCALE") {
                Set(op,TfToken("inputs:uniformScale"),VtValue(Flag(c,"use_make_uniform",false)));
                Set(op,TfToken("inputs:scaleAdd"),VtValue(Flag(c,"use_add",false)));
                Set(op,TfToken("inputs:power"),VtValue(Num(c,"power",1)));
            }
            if(type=="DAMPED_TRACK")Set(op,TfToken("inputs:trackAxis"),VtValue(TfToken(Str(c,"track_axis"))));
            if(type=="STRETCH_TO") {
                Warn("near-zero-length Stretch To retains its incoming frame because native providers require invertible transforms");
                Set(op,TfToken("inputs:keepAxis"),VtValue(TfToken(Str(c,"keep_axis"))));
                Set(op,TfToken("inputs:volume"),VtValue(TfToken(Str(c,"volume"))));
                for(const auto &pair:std::vector<std::pair<const char *,const char *>>{{"rest_length","restLength"},{"bulge","bulge"},{"bulge_min","bulgeMin"},{"bulge_max","bulgeMax"},{"bulge_smooth","bulgeSmooth"}})
                    if(Has(c,pair.first))Set(op,TfToken(std::string("inputs:")+pair.second),VtValue(Number(Field(c,pair.first))));
                Set(op,TfToken("inputs:useBulgeMin"),VtValue(Flag(c,"use_bulge_min",false)));
                Set(op,TfToken("inputs:useBulgeMax"),VtValue(Flag(c,"use_bulge_max",false)));
            }
            op.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
            stackDependencies=dependencies;
            previous=op.GetPath().AppendProperty(TfToken("outputs:matrix"));
        }
        if(Flag(owner.data,"connected",false)) {
            auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("origin_"+owner.name)),TfToken("RigExecConstraintFrame"));
            Set(op,TfToken("inputs:operation"),VtValue(TfToken("PRESERVE_ORIGIN")));
            op.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections({previous});
            op.GetPrim().GetAttribute(TfToken("inputs:origin")).SetConnections({base});
            previous=op.GetPath().AppendProperty(TfToken("outputs:matrix"));
        }
        posed.SetConnections({previous});
    }
    void ConvertConstraint(const JsValue &c,size_t index,bool first) {
        auto &owner=Find(Str(c,"owner"));
        const std::string type=Str(c,"type"),label=owner.id+"/"+Str(c,"name");
        const std::string sourceId=Str(c,"source");
        if(sourceId.empty()) { Warn("constraint has no source: "+label); return; }
        auto &src=Find(sourceId);
        if(type=="SPLINE_IK") {
            if(!first || !SplineIk(owner,c,index))
                Warn("Spline IK curve/hooks or solver settings are unsupported: "+label);
            return;
        }
        if(type=="ARMATURE") {
            bool oneTarget=false;
            if(Has(c,"targets")) {
                size_t active=0;
                for(const auto &target:Array(Field(c,"targets"))) {
                    double weight=Number(Field(target,"weight"));
                    if(weight<0) throw std::runtime_error("negative armature target weight");
                    if(weight>0) { ++active; oneTarget=Str(target,"source")==sourceId; }
                }
                oneTarget=oneTarget && active==1;
            }
            if(!first || !oneTarget || src.kind!="joint" || !Has(src.data,"armature") ||
               owner.kind!="joint" || Flag(c,"use_deform_preserve_volume",false) ||
               Flag(c,"use_bone_envelopes",false) || Number(Field(c,"influence"))!=1 ||
               Num(src.data,"bbone_segments",1)!=1) {
                Warn("armature constraint requires first position, one simple target, full influence and linear/no-envelope mode: "+label); return;
            }
            auto mechanism=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("armature_"+std::to_string(index))),TfToken("RigExecArmatureParent"));
            Set(mechanism,TfToken("inputs:local"),VtValue(owner.rest));
            Set(mechanism,TfToken("inputs:preserveLocation"),VtValue(Flag(owner.data,"connected",false)));
            auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
            SdfPathVector incoming;posed.GetConnections(&incoming);
            if(!incoming.empty()) {
                Set(mechanism,TfToken("inputs:useIncoming"),VtValue(true));
                mechanism.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections(incoming);
            }
            auto &sourceObject=Find(Str(src.data,"armature"));
            Set(mechanism,TfToken("inputs:inverseBind"),VtValue((src.world*sourceObject.world.GetInverse()).GetInverse()));
            SdfPathVector inputs={src.path,sourceObject.path};
            if(!owner.parent.empty()) {
                auto parent=Find(owner.parent).path;
                mechanism.SetRelationship(TfToken("rigExec:parent"),{parent}); inputs.push_back(parent);
            }
            mechanism.SetRelationship(TfToken("rigExec:source"),{src.path});
            mechanism.SetRelationship(TfToken("rigExec:sourceObject"),{sourceObject.path});
            mechanism.SetRelationship(TfToken("rigExec:poseInputs"),inputs);
            for(const auto *channel:{"tx","ty","tz","rx","ry","rz","sx","sy","sz"})
                mechanism.GetPrim().GetAttribute(TfToken(std::string("inputs:")+channel)).SetConnections({owner.controlPath.AppendProperty(TfToken(std::string("avars:")+channel))});
            stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space")).SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            return;
        }
        if(Str(c,"owner_space")!="WORLD" || Str(c,"target_space")!="WORLD" ||
           Flag(c,"use_offset",false) || Flag(c,"invert_x",false) || Flag(c,"invert_y",false) || Flag(c,"invert_z",false) ||
           Flag(c,"use_make_uniform",false) || Flag(c,"use_add",false) || Num(c,"power",1)!=1 ||
           Num(c,"head_tail",0)!=0 || (Has(c,"mix_mode") && Str(c,"mix_mode")!="REPLACE")) {
            Warn("constraint spaces/offset/inversion/mix/head-tail settings are unsupported: "+label); return;
        }
        const std::string name="constraint_"+std::to_string(index);
        if(type=="COPY_TRANSFORMS" && first && Number(Field(c,"influence"))==1 && !Flag(c,"remove_target_shear",false)) {
            auto mechanism=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken("copy_"+std::to_string(index))),TfToken("RigExecCopyFrame"));
            mechanism.SetRelationship(TfToken("rigExec:source"),{src.path});
            mechanism.SetRelationship(TfToken("rigExec:poseInputs"),{src.path});
            bool connected=Flag(owner.data,"connected",false);
            Set(mechanism,TfToken("inputs:preserveLocation"),VtValue(connected));
            auto posed=stage->GetPrimAtPath(owner.path).GetAttribute(TfToken("posed:space"));
            if(connected) {
                SdfPathVector inputs;posed.GetConnections(&inputs);
                if(inputs.empty()) throw std::runtime_error("connected bone requires native base frame");
                mechanism.GetPrim().GetAttribute(TfToken("inputs:incoming")).SetConnections(inputs);
            }
            posed.SetConnections({mechanism.GetPath().AppendProperty(TfToken("outputs:matrix"))});
            return;
        }
        if(type=="IK") {
            if(owner.kind!="joint" || !Flag(c,"use_tail",true) ||
               Flag(c,"use_rotation",false) || !Flag(c,"use_location",true)) {
                Warn("IK requires tail targeting, location, joint owner and no rotation: "+label); return;
            }
            const int count=Has(c,"chain_count") ? Integer(Field(c,"chain_count")) : 0;
            if(count<0) throw std::runtime_error("negative IK chain length");
            Node *first=&owner; int length=1;
            while(!first->parent.empty() && Find(first->parent).kind=="joint" && (!count || length<count)) {
                first=&Find(first->parent); ++length;
            }
            // Blender's terminal bone has its endpoint at its tail. RigExec's
            // inclusive chain ends at a joint origin, so author a virtual tip.
            const SdfPath end=owner.path.AppendChild(TfToken("IkTip_"+std::to_string(index)));
            auto tip=rigExec::RigExecSchemaPrim::Define(stage,end,TfToken("RigExecJoint"));
            Set(tip,TfToken("guide:radius"),VtValue(0.0));
            Set(tip,TfToken("guide:displayOpacity"),VtValue(0.0f));
            GfMatrix4d local(1); local.SetTranslate(GfVec3d(0,Num(owner.data,"length",1),0));
            Set(tip,TfToken("rest:space"),VtValue(local));
            std::vector<SdfPath> poles;
            if(Has(c,"pole")) poles.push_back(Find(Str(c,"pole")).path);
            auto ik=Chain(name).AddSingleChainIkConstraint("solve",first->path,end,src.path,poles);
            ik.SetOrientationMode(TfToken("preserve"));
            double stretch=0;
            if(Flag(c,"use_stretch",true)) {
                std::vector<double> values;
                for(Node *bone=&owner;;bone=&Find(bone->parent)) {
                    values.push_back(Num(bone->data,"ik_stretch",0));
                    if(bone==first)break;
                }
                stretch=*std::max_element(values.begin(),values.end());
                if(std::any_of(values.begin(),values.end(),[&](double value){return value!=stretch;}))
                    Warn("non-uniform per-bone IK stretch requires a weighted solver: "+label);
            }
            ik.GetPrim().GetAttribute(TfToken("inputs:stretch")).Set(static_cast<float>(stretch));
            ik.SetSolverMode(TfToken(poles.empty() ? "singleChain" : "rotatePlane"));
            if(!poles.empty()) ik.SetPoleVectorMode(TfToken("object"));
            // Blender measures pole angle from the root bone's X axis;
            // native rotate-plane IK measures it from the chain bend plane.
            double poleOffset=0;
            if(!poles.empty()) {
                const auto root=first->world.ExtractTranslation();
                const auto tipPosition=local.Transform(GfVec3d(0));
                const auto endPosition=owner.world.Transform(tipPosition);
                const auto axis=(endPosition-root).GetNormalized();
                auto x=first->world.TransformDir(GfVec3d(1,0,0)); x-=axis*GfDot(x,axis);
                Node *second=&owner;
                while(second!=first && second->parent!=first->id)second=&Find(second->parent);
                auto bend=second->world.ExtractTranslation()-root; bend-=axis*GfDot(bend,axis);
                if(bend.GetLength()<1e-8)bend=GfCross(axis,x);
                if(x.GetLength()>1e-8 && bend.GetLength()>1e-8)
                    poleOffset=std::atan2(GfDot(GfCross(x.GetNormalized(),bend.GetNormalized()),axis),
                                         GfDot(x.GetNormalized(),bend.GetNormalized()));
            }
            ik.SetTwistDegrees((Num(c,"pole_angle",0)+poleOffset)*180/3.14159265358979323846);
            ik.SetDefaultWeight(static_cast<float>(Number(Field(c,"influence"))));
            Warn("IK uses native RigExec solve; Blender solver/pole-angle equivalence is not established: "+label);
            return;
        }
        const bool x=Flag(c,"use_x",true),y=Flag(c,"use_y",true),z=Flag(c,"use_z",true);
        rigExec::RigExecSourceConstraintHandle handle;
        if(type=="COPY_LOCATION") {
            if(Flag(owner.data,"connected",false)) return;
            auto op=Chain(name,owner.path).AddPositionConstraint("solve"); op.SetAffectTranslation(x,y,z); handle=op;
        } else if(type=="COPY_ROTATION") {
            auto op=Chain(name,owner.path).AddParentConstraint("solve");
            op.SetAffectTranslation(false,false,false); op.SetAffectRotation(x,y,z); handle=op;
            if(!x || !y || !z) Warn("partial rotation constraint Euler semantics approximated: "+label);
        } else if(type=="COPY_SCALE") {
            auto op=Chain(name,owner.path).AddScaleConstraint("solve"); op.SetAffectScale(x,y,z); handle=op;
        } else if(type=="COPY_TRANSFORMS") {
            auto op=Chain(name,owner.path).AddParentConstraint("solve"); op.SetAffectScale(true,true,true);
            if(Flag(owner.data,"connected",false)) op.SetAffectTranslation(false,false,false);
            handle=op;
        } else { Warn("constraint type is not translated: "+label+" ("+type+")"); return; }
        handle.SetSources({src.path});
        handle.SetReadPhase(TfToken("rigExec:sources"),"final");
        handle.SetDefaultWeight(static_cast<float>(Number(Field(c,"influence"))));
    }
    std::string Asset(const std::string &path) {
        if(path.empty()) return {};
        // Snapshots preserve their original .blend anchor when exported to a
        // different directory. Anonymous snapshots require absolute assets.
        const std::string anchor=Has(scene,"source") && !Str(scene,"source").empty() ? Str(scene,"source") : source;
        const auto id=ArGetResolver().CreateIdentifier(path,ArResolvedPath(anchor));
        if(UsdShadeUdimUtils::IsUdimIdentifier(id)) {
            auto tiles=UsdShadeUdimUtils::ResolveUdimTilePaths(id,{});
            if(tiles.empty())Warn("unresolved UDIM texture: "+id);
            for(const auto &tile:tiles)dependencies.push_back(tile.first);
            return id;
        }
        auto resolved=ArGetResolver().Resolve(id);
        const auto result=resolved.empty() ? id : resolved.GetPathString();
        dependencies.push_back(result);
        if(resolved.empty()) Warn("unresolved asset dependency: "+id);
        return result;
    }
    void Materials() {
        std::map<std::string,std::string> labels;
        for(const auto &data:Array(Field(scene,"materials"))) {
            const auto id=Str(data,"id");
            if(!labels.emplace(id,Has(data,"name") ? Str(data,"name") : id).second)
                throw std::runtime_error("duplicate material ID");
        }
        const auto names=PrimNames(labels);
        for(const auto &data:Array(Field(scene,"materials"))) {
            const auto id=Str(data,"id");
            if(id.empty() || materials.count(id)) throw std::runtime_error("duplicate/empty material ID");
            auto material=UsdShadeMaterial::Define(stage,SdfPath("/Rig/Materials").AppendChild(TfToken(names.at(id))));
            material.GetPrim().SetCustomDataByKey(TfToken("blender:materialId"),VtValue(id));
            material.GetPrim().SetDisplayName(Has(data,"name") ? Str(data,"name") : id);
            auto shader=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken("Surface")));
            shader.CreateIdAttr().Set(TfToken("UsdPreviewSurface"));
            const auto &color=Array(Field(data,"color"));
            if(color.size()!=4) throw std::runtime_error("material color must contain RGBA");
            shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(Float(color[0]),Float(color[1]),Float(color[2])));
            shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).Set(Float(color[3]));
            shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(Float(Field(data,"roughness")));
            shader.CreateInput(TfToken("metallic"),SdfValueTypeNames->Float).Set(Float(Field(data,"metallic")));
            material.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token));
            if(Has(data,"emission"))shader.CreateInput(TfToken("emissiveColor"),SdfValueTypeNames->Color3f).Set(Point(Field(data,"emission")));
            JsObject maps=Has(data,"textures")?Object(Field(data,"textures")):JsObject{};
            const auto texture=Str(data,"texture");
            if(!texture.empty())maps.emplace("diffuseColor",JsValue(JsObject{{"file",JsValue(texture)},{"uv",JsValue("")},{"colorspace",JsValue("sRGB")},{"channel",JsValue("rgb")}}));
            std::map<std::string,std::string> uvLabels;
            for(const auto &entry:maps) uvLabels[Str(entry.second,"uv")]=Str(entry.second,"uv");
            const auto uvNames=PrimNames(uvLabels);
            for(const auto &entry:maps) {
                static const std::set<std::string> inputs{"diffuseColor","roughness","metallic","opacity","normal","emissiveColor","occlusion"};
                if(!inputs.count(entry.first))throw std::runtime_error("unsupported preview texture input");
                const auto &tex=entry.second;const auto uv=Str(tex,"uv");
                auto reader=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken("UV_"+uvNames.at(uv))));
                reader.CreateIdAttr().Set(TfToken("UsdPrimvarReader_float2"));
                reader.CreateInput(TfToken("varname"),SdfValueTypeNames->Token).Set(TfToken(uv.empty()?"st":PropertyName("uv:"+uv)));
                const std::string mapName=entry.first=="diffuseColor"?"Texture":"Texture_"+entry.first;
                auto map=UsdShadeShader::Define(stage,material.GetPath().AppendChild(TfToken(mapName)));
                map.CreateIdAttr().Set(TfToken("UsdUVTexture"));
                map.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).Set(SdfAssetPath(Asset(Str(tex,"file"))));
                map.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(reader.CreateOutput(TfToken("result"),SdfValueTypeNames->Float2));
                map.CreateInput(TfToken("sourceColorSpace"),SdfValueTypeNames->Token).Set(TfToken(Str(tex,"colorspace")));
                map.GetInput(TfToken("file")).GetAttr().SetColorSpace(TfToken(Str(tex,"colorspace")));
                for(const char *name:{"scale","bias"})if(Has(tex,name)) {
                    const auto &v=Array(Field(tex,name));if(v.size()!=4)throw std::runtime_error("texture scale/bias requires four values");
                    map.CreateInput(TfToken(name),SdfValueTypeNames->Float4).Set(GfVec4f(Float(v[0]),Float(v[1]),Float(v[2]),Float(v[3])));
                }
                const auto channel=Str(tex,"channel");
                bool vector=channel=="rgb";
                static const std::set<std::string> scalarChannels{"r","g","b","a"};
                if(!vector && !scalarChannels.count(channel))throw std::runtime_error("invalid texture channel");
                const auto type=entry.first=="normal"?SdfValueTypeNames->Normal3f:vector?SdfValueTypeNames->Color3f:SdfValueTypeNames->Float;
                shader.CreateInput(TfToken(entry.first),type).ConnectToSource(map.CreateOutput(TfToken(channel),vector?SdfValueTypeNames->Float3:SdfValueTypeNames->Float));
            }
            materials.emplace(id,material);
        }
    }
    // Providers with equal (owner, bone, fromBind) evaluate the same frame and
    // are shared. Names derive from the unique node names alone; an alias
    // forces a separate prim where one relationship would repeat a target.
    SdfPath SkinProvider(Node &mesh,Node *bone,bool fromBind,const std::string &alias={}) {
        const auto key=std::make_tuple(mesh.id,bone ? bone->id : std::string(),fromBind);
        if(alias.empty()) { const auto found=skinProviders.find(key); if(found!=skinProviders.end()) return found->second; }
        std::string name=alias;
        if(name.empty()) {
            name=std::string(bone ? "influence" : "follow")+(fromBind ? "_" : "Current_");
            if(bone) {
                // "A_B"+"C" and "A"+"B_C" join alike; a length prefix, which
                // no prim name can start with, keeps such pairs apart.
                if(nodeNames.empty()) for(const auto &entry:nodes) nodeNames.insert(entry.second.name);
                const auto joined=mesh.name+"_"+bone->name;
                for(size_t i=joined.find('_');i!=std::string::npos;i=joined.find('_',i+1))
                    if(i!=mesh.name.size() && nodeNames.count(joined.substr(0,i)) && nodeNames.count(joined.substr(i+1))) {
                        name+=std::to_string(mesh.name.size())+"_"; break;
                    }
                name+=joined;
            } else name+=mesh.name;
        }
        auto provider=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken(name)),TfToken("RigExecControl"));
        Set(provider,TfToken("guide:displayOpacity"),VtValue(0.0f));
        auto expression=rigExec::RigExecSchemaPrim::Define(stage,provider.GetPath().AppendChild(TfToken("Compute")),TfToken("RigExecSkinInfluence"));
        Set(expression,TfToken("inputs:inverseMesh"),VtValue(mesh.world.GetInverse()));
        Set(expression,TfToken("inputs:fromBind"),VtValue(fromBind));
        Set(expression,TfToken("inputs:followOnly"),VtValue(!bone));
        SdfPathVector dependencies;
        if(fromBind) { expression.SetRelationship(TfToken("rigExec:owner"),{mesh.path}); dependencies.push_back(mesh.path); }
        if(bone) {
            auto &sourceObject=Find(Str(bone->data,"armature"));
            Set(expression,TfToken("inputs:inverseBind"),VtValue((bone->world*sourceObject.world.GetInverse()).GetInverse()));
            expression.SetRelationship(TfToken("rigExec:source"),{bone->path});
            expression.SetRelationship(TfToken("rigExec:sourceObject"),{sourceObject.path});
            dependencies.push_back(bone->path);dependencies.push_back(sourceObject.path);
        }
        expression.SetRelationship(TfToken("rigExec:poseInputs"),dependencies);
        provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({expression.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        if(alias.empty()) skinProviders.emplace(key,provider.GetPath());
        return provider.GetPath();
    }
    void SurfaceBinding(Node &node,const JsValue &binding,const SdfPath &target,size_t index) {
        auto &surface=Find(Str(binding,"target"));
        const auto name="surface_"+node.name+"_"+std::to_string(index);
        Chain(name,target);
        auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Movers").AppendChild(TfToken(name)).AppendChild(TfToken("deform")),TfToken("RigExecSurfaceBindingMover"));
        op.ApplyAPI(TfToken("RigExecMoverAPI"));
        op.SetRelationship(TfToken("rigExec:moves"),{target});
        op.SetRelationship(TfToken("rigExec:surface"),{SdfPath("/Rig/Geometry").AppendChild(TfToken(surface.name))});
        op.SetReadPhase(TfToken("rigExec:surface"),"final");
        op.SetRelationship(TfToken("rigExec:frames"),{
            SkinProvider(surface,nullptr,true),SkinProvider(node,nullptr,true,&surface==&node ? name+"_target" : std::string())});
        op.SetReadPhase(TfToken("rigExec:frames"),"final");
        Set(op,TfToken("rigExec:surfaceRestMatrix"),VtValue(surface.world));
        Set(op,TfToken("rigExec:targetRestMatrix"),VtValue(node.world));
        Set(op,TfToken("rigExec:surfaceToBinding"),VtValue(Matrix(Field(binding,"bind_matrix"))));
        const std::pair<const char *,const char *> ints[]={
            {"vertices","vertices"},{"vertex_offsets","vertexOffsets"},
            {"polygon_offsets","polygonOffsets"},{"indices","pointIndices"}};
        for(const auto &entry:ints) {
            VtIntArray values;for(const auto &v:Array(Field(binding,entry.first)))values.push_back(Integer(v));
            Set(op,TfToken(std::string("rigExec:")+entry.second),VtValue(values));
        }
        const std::pair<const char *,const char *> floats[]={
            {"weights","barycentricWeights"},{"influences","bindingWeights"},{"mask","mask"}};
        for(const auto &entry:floats) {
            VtFloatArray values;for(const auto &v:Array(Field(binding,entry.first)))values.push_back(Float(v));
            Set(op,TfToken(std::string("rigExec:")+entry.second),VtValue(values));
        }
        VtVec3fArray offsets;for(const auto &v:Array(Field(binding,"offsets")))offsets.push_back(Point(v));
        Set(op,TfToken("rigExec:offsets"),VtValue(offsets));
        Set(op,TfToken("rigExec:strength"),VtValue(Float(Field(binding,"strength"))));
    }
    void Lattices() {
        if(!Has(scene,"lattices"))return;
        for(const auto &data:Array(Field(scene,"lattices"))) {
            auto &node=Find(Str(data,"id"));
            auto cage=UsdGeomPoints::Define(stage,SdfPath("/Rig/Geometry").AppendChild(TfToken(node.name)));
            cage.GetPrim().SetCustomDataByKey(TfToken("blender:id"),VtValue(node.id));
            cage.CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
            VtVec3fArray points;
            for(const auto &p:Array(Field(data,"points"))) {
                const auto transformed=node.world.Transform(GfVec3d(Point(p)));
                for(int axis=0;axis<3;++axis)if(!std::isfinite(transformed[axis]) || std::abs(transformed[axis])>std::numeric_limits<float>::max())
                    throw std::runtime_error("transformed lattice point overflow");
                points.push_back(GfVec3f(transformed));
            }
            const auto &dimensions=Array(Field(data,"divisions"));
            size_t count=1;
            if(dimensions.size()!=3)throw std::runtime_error("lattice dimensions require three axes");
            for(const auto &d:dimensions) {
                const int n=Integer(d);if(n<1 || size_t(n)>MaxElements/count)throw std::runtime_error("invalid lattice dimensions");
                count*=size_t(n);
            }
            if(points.size()!=count)
                throw std::runtime_error("lattice grid size mismatch");
            cage.CreatePointsAttr().Set(points);
            const auto target=cage.GetPointsAttr().GetPath();
            const auto follow=SkinProvider(node,nullptr,true);
            Chain("latticeFollow_"+node.name,target).AddMatrixMover("follow",follow,{},{},TfToken("final"));
            size_t index=0;
            for(const auto &hook:Array(Field(data,"hooks"))) {
                auto &source=Find(Str(hook,"source"));
                const auto name="latticeHook_"+node.name+"_"+std::to_string(index++);
                auto provider=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(TfToken(name)),TfToken("RigExecControl"));
                Set(provider,TfToken("guide:displayOpacity"),VtValue(0.0f));
                auto expression=rigExec::RigExecSchemaPrim::Define(stage,provider.GetPath().AppendChild(TfToken("Compute")),TfToken("RigExecSkinInfluence"));
                Set(expression,TfToken("inputs:fromBind"),VtValue(false));
                Set(expression,TfToken("inputs:followOnly"),VtValue(false));
                Set(expression,TfToken("inputs:inverseBind"),VtValue(Matrix(Field(hook,"inverse"))));
                expression.SetRelationship(TfToken("rigExec:source"),{source.path});
                expression.SetRelationship(TfToken("rigExec:sourceObject"),{node.path});
                expression.SetRelationship(TfToken("rigExec:poseInputs"),{source.path,node.path});
                provider.GetPrim().GetAttribute(TfToken("posed:space")).SetConnections({expression.GetPath().AppendProperty(TfToken("outputs:matrix"))});
                const float strength=Float(Field(hook,"strength"));
                std::vector<float> weights;for(const auto &w:Array(Field(hook,"mask")))weights.push_back(Float(w)*strength);
                if(weights.size()!=points.size())throw std::runtime_error("lattice hook mask size mismatch");
                const auto weight=rig.AddStaticWeight(name+"_mask",target,weights);
                auto mover=Chain(name,target).AddMatrixMover("hook",provider.GetPath(),weight.GetPath(),{},TfToken("final"));
            }
        }
    }
    void Deformer(Node &node,const JsValue &data,const SdfPath &target,size_t index) {
        const auto kind=Str(data,"type"),name=kind+"_"+node.name+"_"+std::to_string(index);
        if(kind!="deltaMush" && kind!="shrinkwrap" && kind!="lattice")throw std::runtime_error("unknown geometry deformer type: "+kind);
        Chain(name,target);
        const auto type=kind=="deltaMush"?"RigExecDeltaMushMover":kind=="shrinkwrap"?"RigExecSurfaceMover":"RigExecLatticeMover";
        auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Movers").AppendChild(TfToken(name)).AppendChild(TfToken("deform")),TfToken(type));
        op.ApplyAPI(TfToken("RigExecMoverAPI"));op.SetRelationship(TfToken("rigExec:moves"),{target});
        VtFloatArray mask;for(const auto &w:Array(Field(data,"mask")))mask.push_back(Float(w));
        if(kind=="deltaMush") {
            VtVec3fArray rest;for(const auto &p:Array(Field(data,"rest_points")))rest.push_back(Point(p));
            Set(op,TfToken("inputs:restPoints"),VtValue(rest));
            Set(op,TfToken("inputs:iterations"),VtValue(Integer(Field(data,"iterations"))));
            Set(op,TfToken("inputs:step"),VtValue(Float(Field(data,"factor"))));
            Set(op,TfToken("inputs:displacement"),VtValue(Float(Field(data,"detail"))));
            Set(op,TfToken("inputs:pinBorders"),VtValue(Bool(Field(data,"pin_boundary"))));
            Set(op,TfToken("inputs:smoothing"),VtValue(TfToken(Str(data,"smoothing"))));
            Set(op,TfToken("inputs:frameTransport"),VtValue(TfToken("corner")));
            Set(op,TfToken("inputs:onlySmooth"),VtValue(Bool(Field(data,"only_smooth"))));
            Set(op,TfToken("inputs:smoothWeights"),VtValue(mask));
            VtIntArray edges;for(const auto &e:Array(Field(data,"edges")))edges.push_back(Integer(e));
            Set(op,TfToken("inputs:edges"),VtValue(edges));
            Set(op,TfToken("inputs:computationToTarget"),VtValue(node.world));
            op.SetRelationship(TfToken("rigExec:frame"),{SkinProvider(node,nullptr,true)});
            op.SetReadPhase(TfToken("rigExec:frame"),"final");
            return;
        }
        auto &dependency=Find(Str(data,kind=="shrinkwrap"?"target":"cage"));
        Set(op,TfToken("rigExec:pointSpace"),VtValue(TfToken("common")));
        Set(op,TfToken("rigExec:targetMatrix"),VtValue(node.world));
        Set(op,TfToken("rigExec:mask"),VtValue(mask));
        op.SetRelationship(TfToken("rigExec:frames"),{SkinProvider(dependency,nullptr,true),SkinProvider(node,nullptr,true,&dependency==&node ? name+"_owner" : std::string())});
        op.SetReadPhase(TfToken("rigExec:frames"),"final");
        const auto dependencyPath=SdfPath("/Rig/Geometry").AppendChild(TfToken(dependency.name));
        if(kind=="shrinkwrap") {
            op.SetRelationship(TfToken("rigExec:surface"),{dependencyPath});op.SetReadPhase(TfToken("rigExec:surface"),"final");
            Set(op,TfToken("rigExec:surfaceMatrix"),VtValue(dependency.world));
            Set(op,TfToken("rigExec:snapMode"),VtValue(TfToken(Str(data,"mode"))));
            Set(op,TfToken("rigExec:offset"),VtValue(Float(Field(data,"offset"))));
            VtIntArray triangles;for(const auto &v:Array(Field(data,"triangles")))triangles.push_back(Integer(v));
            Set(op,TfToken("rigExec:triangles"),VtValue(triangles));
        } else {
            const JsValue *lattice=nullptr;
            for(const auto &candidate:Array(Field(scene,"lattices")))if(Str(candidate,"id")==dependency.id){lattice=&candidate;break;}
            if(!lattice)throw std::runtime_error("missing lattice data");
            const auto &dims=Array(Field(*lattice,"divisions")),&interpolation=Array(Field(*lattice,"interpolation"));
            if(dims.size()!=3||interpolation.size()!=3)throw std::runtime_error("lattice dimensions/interpolation mismatch");
            const std::map<std::string,std::string> modes{{"KEY_LINEAR","linear"},{"KEY_BSPLINE","bspline"},{"KEY_CARDINAL","cardinal"},{"KEY_CATMULL_ROM","catmullRom"}};
            op.SetRelationship(TfToken("rigExec:cage"),{dependencyPath});op.SetReadPhase(TfToken("rigExec:cage"),"final");
            Set(op,TfToken("rigExec:evaluation"),VtValue(TfToken("regularGrid")));
            Set(op,TfToken("rigExec:divisions"),VtValue(GfVec3i(Integer(dims[0]),Integer(dims[1]),Integer(dims[2]))));
            Set(op,TfToken("rigExec:origin"),VtValue(Point(Field(*lattice,"origin"))));
            Set(op,TfToken("rigExec:spacing"),VtValue(Point(Field(*lattice,"spacing"))));
            Set(op,TfToken("rigExec:cageMatrix"),VtValue(dependency.world));
            Set(op,TfToken("rigExec:strength"),VtValue(Float(Field(data,"strength"))));
            for(int axis=0;axis<3;++axis)Set(op,TfToken(std::string("rigExec:interpolation")+"UVW"[axis]),VtValue(TfToken(modes.at(String(interpolation[axis])))));
        }
    }
    void ShapeKeys(Node &node,const JsValue &data,const UsdGeomMesh &mesh,size_t pointCount) {
        if(!Has(data,"shape_keys") || Array(Field(data,"shape_keys")).empty())return;
        auto op=Chain("shape_"+node.name,mesh.GetPath().AppendProperty(TfToken("points"))).AddBlendShapeMover("deform");
        std::set<std::string> names;
        for(const auto &key:Array(Field(data,"shape_keys"))) {
            const std::string name=Str(key,"name"),encoded=PropertyName(name);
            if(!names.insert(name).second)throw std::runtime_error("duplicate shape key name");
            const float minimum=Float(Field(key,"minimum")),maximum=Float(Field(key,"maximum")),value=Float(Field(key,"value"));
            if(minimum>maximum)
                throw std::runtime_error("invalid shape key range/value");
            const auto &deltas=Array(Field(key,"deltas"));
            if(deltas.size()!=pointCount)throw std::runtime_error("shape key point count mismatch");
            auto weight=mesh.GetPrim().CreateAttribute(TfToken("blender:shapeKey:"+encoded),SdfValueTypeNames->Float);
            weight.Set(value);weight.SetDisplayName(name);
            weight.SetCustomDataByKey(TfToken("blender:shapeKey"),VtValue(name));
            weight.SetCustomDataByKey(TfToken("blender:minimum"),VtValue(minimum));
            weight.SetCustomDataByKey(TfToken("blender:maximum"),VtValue(maximum));
            weight.SetCustomDataByKey(TfToken("blender:mute"),VtValue(Flag(key,"mute",false)));
            for(int sign:{1,-1}) {
                const float activation=sign>0?std::max({1.f,maximum,value}):std::max({0.f,-minimum,-value});
                if(activation<=0)continue;
                const std::string branch=encoded+(sign>0?"_positive":"_negative");
                auto input=op.AddBlendInput(branch);
                SdfPath source=weight.GetPath();
                if(sign<0) {
                    auto negative=mesh.GetPrim().CreateAttribute(TfToken("blender:shapeKeyNegative:"+encoded),SdfValueTypeNames->Float);
                    negative.Set(-1.f);
                    auto invert=Chain("shape_weight_"+node.name+"_"+encoded,negative.GetPath()).AddFloatMathMover("invert",TfToken("multiply"),value);
                    invert.GetPrim().GetAttribute(TfToken("inputs:value")).SetConnections({source});source=negative.GetPath();
                }
                input.ConnectWeight(source);
                // The inverted weight is a mover result; the default read phase sees its authored value.
                if(sign<0)rigExec::RigExecSchemaPrim::Get(stage,input.GetPath(),TfToken("RigExecBlendInput")).SetReadPhase(TfToken("inputs:weight"),"final");
                VtVec3fArray offsets;VtIntArray indices;
                for(size_t i=0;i<deltas.size();++i) {
                    const auto delta=node.world.TransformDir(GfVec3d(Point(deltas[i]))) * (sign*activation);
                    for(int axis=0;axis<3;++axis)if(!std::isfinite(delta[axis]) || std::abs(delta[axis])>std::numeric_limits<float>::max())
                        throw std::runtime_error("transformed shape key overflow");
                    if(delta!=GfVec3d(0)) {indices.push_back(static_cast<int>(i));offsets.emplace_back(delta);}
                }
                // Empty sparse indices mean a dense sample in the native contract.
                if(indices.empty())offsets.assign(pointCount,GfVec3f(0));
                auto shape=UsdSkelBlendShape::Define(stage,op.GetPath().AppendChild(TfToken(branch+"_shape")));
                shape.CreateOffsetsAttr().Set(offsets);shape.CreatePointIndicesAttr().Set(indices);
                input.AddSample("sample",activation).SetBlendShape(shape.GetPath());
            }
        }
    }
    void Geometry() {
        std::set<std::string> ids;
        for(const auto &data:Array(Field(scene,"meshes"))) {
            const auto id=Str(data,"id"); auto &node=Find(id);
            if(!ids.insert(id).second) throw std::runtime_error("duplicate mesh ID");
            auto mesh=UsdGeomMesh::Define(stage,SdfPath("/Rig/Geometry").AppendChild(TfToken(node.name)));
            mesh.GetPrim().SetDisplayName(Str(node.data,"name"));
            mesh.GetPrim().SetCustomDataByKey(TfToken("blender:id"),VtValue(id));
            VtVec3fArray points; VtIntArray counts,indices; size_t corners=0;
            for(const auto &p:Array(Field(data,"points"))) {
                const GfVec3d point=node.world.Transform(GfVec3d(Point(p)));
                for(int axis=0;axis<3;++axis) if(!std::isfinite(point[axis]) || std::abs(point[axis])>std::numeric_limits<float>::max())
                    throw std::runtime_error("transformed point overflow");
                points.push_back(GfVec3f(point));
            }
            for(const auto &c:Array(Field(data,"counts"))) {
                const int count=Integer(c); if(count<3) throw std::runtime_error("polygon has fewer than 3 corners");
                counts.push_back(count); corners+=count;
            }
            if(corners>MaxElements) throw std::runtime_error("too many mesh corners");
            for(const auto &v:Array(Field(data,"indices"))) {
                const int index=Integer(v); if(index<0 || static_cast<size_t>(index)>=points.size()) throw std::runtime_error("mesh index out of range");
                indices.push_back(index);
            }
            if(corners!=indices.size()) throw std::runtime_error("mesh topology size mismatch");
            mesh.CreatePointsAttr().Set(points); mesh.CreateFaceVertexCountsAttr().Set(counts);
            mesh.CreateFaceVertexIndicesAttr().Set(indices); mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->catmullClark);
            if(!points.empty()) {
                GfVec3f lower=points[0],upper=points[0];
                for(const auto &point:points) for(int axis=0;axis<3;++axis) { lower[axis]=std::min(lower[axis],point[axis]); upper[axis]=std::max(upper[axis],point[axis]); }
                mesh.CreateExtentAttr().Set(VtVec3fArray{lower,upper});
            }
            if(!Flag(node.data,"visible",true)) mesh.CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
            const auto &uv=Array(Field(data,"uv"));
            if(!uv.empty()) {
                if(uv.size()!=corners) throw std::runtime_error("UV corner count mismatch");
                VtVec2fArray values;
                for(const auto &value:uv) {
                    const auto &pair=Array(value); if(pair.size()!=2) throw std::runtime_error("UV must contain 2 numbers");
                    values.push_back(GfVec2f(Float(pair[0]),Float(pair[1])));
                }
                UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->faceVarying).Set(values);
            }
            if(Has(data,"uv_sets"))for(const auto &entry:Object(Field(data,"uv_sets"))) {
                const auto &uvs=Array(entry.second);if(uvs.size()!=corners)throw std::runtime_error("named UV corner count mismatch");
                VtVec2fArray values;
                for(const auto &value:uvs) {const auto &pair=Array(value);if(pair.size()!=2)throw std::runtime_error("UV must contain 2 numbers");values.emplace_back(Float(pair[0]),Float(pair[1]));}
                UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken(PropertyName("uv:"+entry.first)),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->faceVarying).Set(values);
            }
            // Smooth flags stay in the snapshot, but are not equivalent
            // to a USD mesh without Blender's split-normal evaluation.
            const auto &smooth=Array(Field(data,"smooth"));
            if(smooth.size()!=counts.size()) throw std::runtime_error("smooth flag count mismatch");
            if(std::any_of(smooth.begin(),smooth.end(),[](const JsValue &v){return Bool(v);}))
                Warn("smooth shading normals require host recomputation: "+id);
            const auto &slots=Array(Field(data,"materials"));
            const auto &faceMaterials=Array(Field(data,"material_indices"));
            if(faceMaterials.size()!=counts.size()) throw std::runtime_error("material face count mismatch");
            std::map<int,VtIntArray> assignments;
            for(size_t i=0;i<faceMaterials.size();++i) {
                int slot=Integer(faceMaterials[i]);
                if(slot<0 || (slots.empty() ? slot!=0 : static_cast<size_t>(slot)>=slots.size())) throw std::runtime_error("material slot out of range");
                if(!slots.empty()) assignments[slot].push_back(static_cast<int>(i));
            }
            for(const auto &entry:assignments) {
                const auto matId=String(slots[entry.first]); if(matId.empty()) continue;
                if(!materials.count(matId)) throw std::runtime_error("unresolved material: "+matId);
                if(entry.second.size()==counts.size()) UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(materials.at(matId));
                else {
                    auto subset=UsdGeomSubset::CreateGeomSubset(mesh,TfToken("material_"+std::to_string(entry.first)),
                        UsdGeomTokens->face,entry.second,TfToken("materialBind"),UsdGeomTokens->nonOverlapping);
                    UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(materials.at(matId));
                }
            }
            JsArray skins;
            if(Has(data,"deformers")) skins=Array(Field(data,"deformers"));
            else if(Has(data,"skin_stack")) skins=Array(Field(data,"skin_stack"));
            else if(!Field(data,"skin").IsNull()) skins.push_back(Field(data,"skin"));
            if(!Has(data,"deformers") && Has(data,"surface_bindings")) {
                for(const auto &binding:Array(Field(data,"surface_bindings")))skins.push_back(binding);
                std::stable_sort(skins.begin(),skins.end(),[](const JsValue &a,const JsValue &b) {
                    return (Has(a,"modifier_index")?Integer(Field(a,"modifier_index")):0) <
                           (Has(b,"modifier_index")?Integer(Field(b,"modifier_index")):0);
                });
            }
            ShapeKeys(node,data,mesh,points.size());
            if(skins.empty() || Has(skins.front(),"bind_matrix") || (Has(skins.front(),"type") && Str(skins.front(),"type")!="skin")) {
                auto follow=SkinProvider(node,nullptr,true);
                Chain("mesh_"+node.name,mesh.GetPath().AppendProperty(TfToken("points"))).AddMatrixMover("follow",follow,{},{},TfToken("final"));
            }
            for(size_t skinIndex=0;skinIndex<skins.size();++skinIndex) {
            const auto &skin=skins[skinIndex];
            if(Has(skin,"type") && Str(skin,"type")!="skin" && Str(skin,"type")!="surfaceBinding") {
                Deformer(node,skin,mesh.GetPath().AppendProperty(TfToken("points")),skinIndex);
                continue;
            }
            if(Has(skin,"bind_matrix")) {
                SurfaceBinding(node,skin,mesh.GetPath().AppendProperty(TfToken("points")),skinIndex);
                continue;
            }
            const auto method=Str(skin,"method");
            if(method!="classicLinear" && method!="dualQuaternion") throw std::runtime_error("unsupported skinning method: "+method);
            std::vector<SdfPath> palette;
            std::vector<Node *> bones;
            bool objectSpace=true;
            for(const auto &influence:Array(Field(skin,"influences"))) {
                auto &bone=Find(String(influence));bones.push_back(&bone);
                palette.push_back(bone.path);objectSpace=objectSpace && Has(bone.data,"armature");
            }
            if(palette.empty()) { Warn("skin has no deforming bones: "+id); continue; }
            const size_t boneCount=palette.size();
            const std::string prefix="influenceSlot_"+node.name+"_"+std::to_string(skinIndex)+"_";
            const bool fromBase=Flag(skin,"use_base_input",false),fromBind=skinIndex==0 || fromBase;
            if(objectSpace) {
                std::set<Node *> listed;
                for(size_t i=0;i<bones.size();++i)
                    palette[i]=SkinProvider(node,bones[i],fromBind,listed.insert(bones[i]).second ? std::string() : prefix+std::to_string(i));
            }
            bool unweighted=false;
            const auto &rows=Array(Field(skin,"weights"));
            if(rows.size()!=points.size()) throw std::runtime_error("skin vertex count mismatch");
            size_t slotsPerPoint=1;
            for(const auto &row:rows) slotsPerPoint=std::max(slotsPerPoint,Array(row).size());
            if(points.size() && slotsPerPoint>MaxElements/points.size()) throw std::runtime_error("skin layout too large");
            std::vector<int> jointIndices(points.size()*slotsPerPoint,0);
            std::vector<float> weights(jointIndices.size(),0);
            for(size_t i=0;i<rows.size();++i) {
                std::set<int> seen; size_t j=0; double total=0;
                for(const auto &entry:Array(rows[i])) {
                    const auto &pair=Array(entry); if(pair.size()!=2) throw std::runtime_error("skin entry must be [index,weight]");
                    const int index=Integer(pair[0]); const float weight=Float(pair[1]);
                    if(index<0 || static_cast<size_t>(index)>=boneCount || weight<0 || !seen.insert(index).second)
                        throw std::runtime_error("invalid skin influence/weight");
                    jointIndices[i*slotsPerPoint+j]=index; weights[i*slotsPerPoint+j]=weight; total+=weight; ++j;
                }
                if(total>0) for(size_t j=0;j<slotsPerPoint;++j) weights[i*slotsPerPoint+j]=static_cast<float>(weights[i*slotsPerPoint+j]/total);
                else if(objectSpace) {jointIndices[i*slotsPerPoint]=static_cast<int>(boneCount);weights[i*slotsPerPoint]=1;unweighted=true;}
            }
            const std::string chainName="skin_"+node.name+"_"+std::to_string(skinIndex);
            const auto target=mesh.GetPath().AppendProperty(TfToken("points"));
            auto chain=Chain(chainName,target);
            VtFloatArray mask;
            if(Has(skin,"mask")) for(const auto &v:Array(Field(skin,"mask"))) {
                float w=Float(v); if(w<0 || w>1) throw std::runtime_error("skin mask outside [0,1]");
                mask.push_back(w);
            }
            if(!mask.empty() && mask.size()!=points.size()) throw std::runtime_error("skin mask vertex count mismatch");
            const bool layered=!mask.empty() || fromBase,transformInput=layered && objectSpace && skinIndex==0;
            // The owner-follow slot exists only for zero-weight vertices and
            // for the layered transform input.
            if(objectSpace && (unweighted || transformInput)) palette.push_back(SkinProvider(node,nullptr,fromBind));
            for(const int index:jointIndices) if(index<0 || static_cast<size_t>(index)>=palette.size())
                throw std::runtime_error("skin index references a missing palette slot");
            if(!layered) {
                auto op=chain.AddSkinMover("deform",palette,{},{},TfToken("final"));
                op.SetJointInfluences(jointIndices,weights,static_cast<int>(slotsPerPoint));
                op.SetSkinningMethod(TfToken(Str(skin,"method")));
            } else {
                auto op=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Movers").AppendChild(TfToken(chainName)).AppendChild(TfToken("deform")),TfToken("RigExecLayeredSkinMover"));
                op.ApplyAPI(TfToken("RigExecMoverAPI"));
                op.SetRelationship(TfToken("rigExec:moves"),{target});
                op.SetRelationship(TfToken("rigExec:influences"),SdfPathVector(palette.begin(),palette.end()));
                op.SetReadPhase(TfToken("rigExec:influences"),"final");
                Set(op,TfToken("rigExec:jointIndices"),VtValue(VtIntArray(jointIndices.begin(),jointIndices.end())));
                Set(op,TfToken("rigExec:jointWeights"),VtValue(VtFloatArray(weights.begin(),weights.end())));
                Set(op,TfToken("rigExec:elementSize"),VtValue(static_cast<int>(slotsPerPoint)));
                Set(op,TfToken("rigExec:skinningMethod"),VtValue(TfToken(Str(skin,"method"))));
                Set(op,TfToken("inputs:mask"),VtValue(mask));
                Set(op,TfToken("inputs:useBaseInput"),VtValue(fromBase));
                if(transformInput) {
                    Set(op,TfToken("inputs:transformInput"),VtValue(true));
                    op.SetRelationship(TfToken("rigExec:transform"),{palette.back()});
                    op.SetReadPhase(TfToken("rigExec:transform"),"final");
                }
            }
            }
        }
    }
    // Native biped slot role -> source control names, by rig-generator
    // convention only: a name is listed because a generator emits it for its
    // own metarig bones, never because one character uses it. Everything else
    // is reached through the overflow panels.
    //   Rigify (rig types and metarigs bundled with Blender): spines.basic_spine
    //   torso, hips, chest, <bone>_fk, tweak_<bone>, torso_pivot;
    //   spines.super_head neck, head; basic.super_copy shoulder; limbs.arm,
    //   limbs.leg and limbs.paw <bone>_fk, <bone>_ik, <bone>_ik_target,
    //   <bone>_parent, <bone>_tweak, foot_heel_ik; limbs.super_finger <bone>;
    //   limbs.super_palm <bone>_fk; faces.super_face and its skin upgrade
    //   (face.skin_eye, face.skin_jaw, skin chains with an _end control).
    //   CloudRig (component prefix on a bone of its bundled metarigs, whose
    //   spine is Spine, RibCage, Chest; Spine1..3 in 4.5; Hips, Spine, Chest
    //   on main, and whose hand is Wrist, Hand on main):
    //   cloud_spine TORSO-, HIP-, CHST-<spine> (MSTR-<spine>_Torso, _Hips,
    //   _Chest before 4.1); cloud_fk_chain FK-, FK-OS-, ROOT-; cloud_ik_chain
    //   IK- (IK-MSTR-), POLE- (IK-POLE-<bone>, IK-POLE-Arm, IK-POLE-Leg), FK-W-;
    //   cloud_leg ROLL-M-, ROLL-; cloud_chain STR-<bone>, STR-<bone>_<n>
    //   (STR-<bone><n>); cloud_aim CTR-, TGT-<bone> and TGT-<group>
    //   (MSTR-TGT-<group>); ACT- bones of the bundled face metarig.
    // Names are compared without their side token and ignoring case, Rigify
    // before CloudRig, in the order written. Sided roles resolve once per
    // side as L_/R_.
    struct PickerRole { std::string role; bool sided; std::vector<std::string> names; };
    static const std::vector<PickerRole> &PickerRoles() {
        static const std::vector<PickerRole> table=[] {
            std::vector<PickerRole> out;
            const auto add=[&out](const char *role,bool sided,const std::string &rigify,const std::string &cloudRig) {
                PickerRole entry{role,sided,{}};
                for(const auto *names:{&rigify,&cloudRig}) {
                    std::istringstream stream(*names);
                    for(std::string name;std::getline(stream,name,'|');) {
                        for(char &c:name) if(c>='A' && c<='Z')c=char(c-'A'+'a');
                        entry.names.push_back(name);
                    }
                }
                out.push_back(std::move(entry));
            };
            add("Main",false,"root","root");
            add("M_Body",false,"torso","TORSO-Spine|TORSO-Spine1|TORSO-Hips|MSTR-Spine_Torso");
            add("M_BodyGimbal",false,"torso_pivot","");
            add("M_HipSwivel",false,"hips","HIP-Spine|HIP-Spine1|HIP-Hips|MSTR-Spine_Hips");
            add("M_Chest",false,"chest","CHST-Spine|CHST-Spine1|CHST-Hips|MSTR-Spine_Chest");
            // A spine chain is Spine, RibCage, Chest; Spine1..3; or Hips, Spine,
            // Chest: the lowest role resolves first, so Spine is the middle
            // bone only where the chain starts at Hips.
            add("M_Torso",false,"spine_fk.001|spine_fk","FK-Hips|FK-OS-Spine|FK-Spine|FK-OS-Spine1|FK-Spine1");
            add("M_SpineMid",false,"spine_fk.002","FK-OS-RibCage|FK-RibCage|FK-OS-Spine2|FK-Spine2|FK-Spine");
            add("M_ChestTop",false,"spine_fk.003","FK-OS-Chest|FK-Chest|FK-OS-Spine3|FK-Spine3");
            add("M_SpineBend1",false,"tweak_spine.001","STR-Hips|STR-Spine|STR-Spine1");
            add("M_SpineBend2",false,"tweak_spine.002","STR-RibCage|STR-Spine2|STR-Spine");
            add("M_SpineBend3",false,"tweak_spine.003","STR-Chest|STR-Spine3");
            add("M_Neck",false,"neck","FK-Neck");
            add("M_NeckShift",false,"","STR-Neck");
            add("M_Head",false,"head","FK-Head");
            add("M_HeadGimbal",false,"","STR-Head");
            add("Shldr",true,"shoulder","FK-Shoulder");
            add("UpArm",true,"upper_arm_fk","FK-UpperArm");
            add("LoArm",true,"forearm_fk","FK-Forearm");
            add("Hand",true,"hand_fk","FK-Wrist|FK-Hand");
            add("ArmIK",true,"hand_ik","IK-Wrist|IK-Hand|IK-MSTR-Wrist");
            add("ArmPV",true,"upper_arm_ik_target","POLE-UpperArm|IK-POLE-UpperArm|IK-POLE-Arm");
            add("Arm",true,"upper_arm_parent","");
            add("UpArmSwing",true,"upper_arm_ik","ROOT-UpperArm");
            add("ArmBend0",true,"upper_arm_tweak","STR-UpperArm_1|STR-UpperArm1");
            add("ArmBend1",true,"upper_arm_tweak.001","STR-UpperArm_2|STR-UpperArm2");
            add("ArmBend2",true,"forearm_tweak","STR-Forearm_1|STR-Forearm1");
            add("ArmBend3",true,"forearm_tweak.001","STR-Forearm_2|STR-Forearm2");
            add("ArmBend4",true,"hand_tweak","STR-Wrist|STR-Hand");
            add("UpLeg",true,"thigh_fk","FK-Thigh");
            add("LoLeg",true,"shin_fk","FK-Knee");
            add("Foot",true,"foot_fk","FK-W-Foot|FK-Foot");
            add("Toe",true,"toe_fk|toe|r_toe","FK-Toes");
            add("LegIK",true,"foot_ik","IK-Foot|IK-MSTR-Foot");
            add("LegPV",true,"thigh_ik_target","POLE-Thigh|IK-POLE-Thigh|IK-POLE-Leg");
            add("Leg",true,"thigh_parent","");
            add("UpLegSwing",true,"thigh_ik","ROOT-Thigh");
            add("Heel",true,"foot_heel_ik","");
            add("BallRoll",true,"","ROLL-M-Foot|ROLL-Foot");
            add("LegBend0",true,"thigh_tweak","STR-Thigh_1|STR-Thigh1");
            add("LegBend1",true,"thigh_tweak.001","STR-Thigh_2|STR-Thigh2");
            add("LegBend2",true,"shin_tweak","STR-Knee_1|STR-Knee1");
            add("LegBend3",true,"shin_tweak.001","STR-Knee_2|STR-Knee2");
            add("LegBend4",true,"foot_tweak","STR-Foot");
            add("ThumbBase",true,"thumb.01","FK-Finger_Thumb1");
            add("ThumbMid",true,"thumb.02","FK-Finger_Thumb2");
            add("ThumbTip",true,"thumb.03","FK-Finger_Thumb3");
            add("IndexBase",true,"f_index.01","FK-Finger_Index1");
            add("IndexMid",true,"f_index.02","FK-Finger_Index2");
            add("IndexTip",true,"f_index.03","FK-Finger_Index3");
            add("MiddleBase",true,"f_middle.01","FK-Finger_Middle1");
            add("MiddleMid",true,"f_middle.02","FK-Finger_Middle2");
            add("MiddleTip",true,"f_middle.03","FK-Finger_Middle3");
            add("RingBase",true,"f_ring.01","FK-Finger_Ring1");
            add("RingMid",true,"f_ring.02","FK-Finger_Ring2");
            add("RingTip",true,"f_ring.03","FK-Finger_Ring3");
            add("PinkyBase",true,"f_pinky.01","FK-Finger_Pinky1");
            add("PinkyMid",true,"f_pinky.02","FK-Finger_Pinky2");
            add("PinkyTip",true,"f_pinky.03","FK-Finger_Pinky3");
            add("IndexMeta",true,"palm.01_fk","FK-Finger_Index_Carpal");
            add("MiddleMeta",true,"palm.02_fk","FK-Finger_Middle_Carpal");
            add("RingMeta",true,"palm.03_fk","FK-Finger_Ring_Carpal");
            add("PinkyMeta",true,"palm.04_fk","FK-Finger_Pinky_Carpal");
            add("M_Look",false,"eye_common|eyes","TGT-Eyes|MSTR-TGT-Eyes");
            add("Eye",true,"eye|eye_master","CTR-Eye|TGT-Eye");
            add("M_Jaw",false,"jaw_master|jaw","Jaw");
            add("M_Chin",false,"chin","");
            add("M_Mouth",false,"jaw_master_mouth","");
            add("teeth_upper_def",false,"teeth.T","");
            add("teeth_lower_def",false,"teeth.B","");
            add("M_Nose",false,"nose_master|nose","");
            add("M_NoseBridge",false,"nose.001","");
            add("M_Septum",false,"nose.004","");
            add("Brow",true,"brow.T.002","ACT-Eyebrow_Main");
            add("InBrow",true,"brow.T.003","");
            add("MidBrow",true,"brow.T.001","");
            add("UpLid",true,"lid.T.002","ACT-Eyelid_Upper");
            add("LoLid",true,"lid.B.002","ACT-Eyelid_Lower");
            // Both lid chains run from their first bone: lid.T from the outer
            // corner over the eye, lid.B from the inner corner under it.
            add("InLid",true,"lid.B","");
            add("OutLid",true,"lid.T","");
            add("M_UpLipMid",false,"lip.T","");
            add("M_LoLipMid",false,"lip.B","");
            add("UpLip1",true,"lip.T.001","");
            add("LoLip1",true,"lip.B.001","");
            add("UpLip2",true,"lip.T.002","");
            add("LoLip2",true,"lip.B.002","");
            add("UpLip3",true,"lip.T.003","");
            add("LoLip3",true,"lip.B.003","");
            add("OutLip",true,"lips|lip_end.001|lip_end.002|lip_end.003","");
            add("Mouth",true,"","ACT-MouthCorner");
            add("Cheek",true,"cheek.B.001","");
            add("CheekInner",true,"cheek.T.001","");
            add("Ear",true,"ear","");
            return out;
        }();
        return table;
    }
    // "hand_ik.L", "brow.T.L.002", "Left Arm" -> side and the lower-case name without it.
    static std::pair<char,std::string> PickerSide(const std::string &name) {
        const auto lower=[](std::string text) {
            for(char &c:text) if(c>='A' && c<='Z')c=char(c-'A'+'a');
            return text;
        };
        for(size_t i=0;i+1<name.size();++i)
            if((name[i]=='.' || name[i]=='_' || name[i]==' ') && (name[i+1]=='L' || name[i+1]=='R') &&
               (i+2==name.size() || name[i+2]=='.' || name[i+2]=='_'))
                return {name[i+1],lower(name.substr(0,i)+name.substr(i+2))};
        for(const std::string word:{"Left","Right"})
            if(name.size()>word.size() && name.compare(0,word.size(),word)==0 &&
               (name[word.size()]==' ' || name[word.size()]=='_'))
                return {word[0],lower(name.substr(word.size()+1))};
        return {'M',lower(name)};
    }
    // Copy the Body/Face panels whose slots resolve to current picker targets
    // of this armature; returns the controls that own a single-target slot.
    // Unresolved slots are not authored. A bone that is neither visible nor
    // shaped is a mechanism the source merely lists and takes no slot;
    // visible, then shaped controls rank first, then table order.
    std::set<SdfPath> BipedPicker(const Node &owner,const SdfPath &path,const std::vector<Node *> &targets) {
        std::set<SdfPath> placed;
        const auto shaped=[](const Node &node) { return Has(node.data,"guide_points") && !Array(Field(node.data,"guide_points")).empty(); };
        std::set<std::string> names;
        std::map<std::pair<char,std::string>,std::vector<Node *>> byStem;
        for(Node *node:targets) {
            if(node->kind!="joint" || !Has(node->data,"armature") || Str(node->data,"armature")!=owner.id)continue;
            if(!Flag(node->data,"visible",true) && !shaped(*node))continue;
            const auto name=Str(node->data,"name");
            if(names.insert(name).second)byStem[PickerSide(name)].push_back(node);
        }
        if(names.empty())return placed;
        const auto plugin=PlugRegistry::GetInstance().GetPluginWithName("usdBlenderRig");
        if(!plugin)throw std::runtime_error("native biped picker resource plugin unavailable");
        const auto resource=SdfLayer::FindOrOpen(plugin->GetResourcePath()+"/biped_picker.usda");
        const auto source=resource ? resource->GetPrimAtPath(SdfPath("/Template")) : SdfPrimSpecHandle();
        if(!source)throw std::runtime_error("native biped picker resource unavailable");
        // The template names roles only; which control fills a role is the table's decision.
        struct Slot { SdfPath path; std::vector<std::string> roles; };
        std::vector<Slot> slots;
        for(const auto &panel:source->GetNameChildren()) for(const auto &button:panel->GetNameChildren()) {
            const VtValue custom=button->GetInfo(SdfFieldKeys->CustomData);
            if(!custom.IsHolding<VtDictionary>())continue;
            const auto found=custom.UncheckedGet<VtDictionary>().find("blender");
            if(found==custom.UncheckedGet<VtDictionary>().end() || !found->second.IsHolding<VtDictionary>())continue;
            const auto &blender=found->second.UncheckedGet<VtDictionary>();
            const auto roles=blender.find("roles");
            if(roles==blender.end() || !roles->second.IsHolding<VtStringArray>())continue;
            const auto &value=roles->second.UncheckedGet<VtStringArray>();
            slots.push_back({button->GetPath(),{value.begin(),value.end()}});
        }
        std::map<std::string,Node *> roleBone;
        std::set<Node *> used;
        for(const auto &entry:PickerRoles()) for(const char side:std::string(entry.sided ? "LR" : "M")) {
            Node *best=nullptr;
            int bestRank=0;
            for(size_t i=0;i<entry.names.size();++i) {
                const auto found=byStem.find({side,entry.names[i]});
                if(found==byStem.end())continue;
                for(Node *node:found->second) {
                    const int value=(Flag(node->data,"visible",true) ? 0 : 20000)+(shaped(*node) ? 0 : 10000)+int(i);
                    if(!used.count(node) && (!best || value<bestRank)) { best=node; bestRank=value; }
                }
            }
            if(best) { roleBone[entry.sided ? std::string(1,side)+"_"+entry.role : entry.role]=best; used.insert(best); }
        }
        std::map<SdfPath,SdfPathVector> live;
        for(const auto &slot:slots) {
            SdfPathVector resolved;
            for(const auto &role:slot.roles) {
                const auto found=roleBone.find(role);
                if(found!=roleBone.end())resolved.push_back(found->second->controlPath);
            }
            // A group slot that would select one control only repeats that control's own slot.
            if(resolved.empty() || (slot.roles.size()>1 && resolved.size()<2))continue;
            if(slot.roles.size()==1)placed.insert(resolved.front());
            live.emplace(slot.path,std::move(resolved));
        }
        const SdfPath templatePath("/Template");
        std::set<SdfPath> roleSlots;
        for(const auto &slot:slots)roleSlots.insert(slot.path);
        for(const auto &panel:source->GetNameChildren()) {
            bool any=false;
            for(const auto &button:panel->GetNameChildren())any=any || live.count(button->GetPath());
            const auto panelPath=panel->GetPath().ReplacePrefix(templatePath,path);
            if(!any || !SdfCopySpec(resource,panel->GetPath(),stage->GetRootLayer(),panelPath))continue;
            const auto copy=stage->GetRootLayer()->GetPrimAtPath(panelPath);
            for(const auto &button:panel->GetNameChildren()) {
                const auto spec=stage->GetRootLayer()->GetPrimAtPath(panelPath.AppendChild(button->GetNameToken()));
                if(roleSlots.count(button->GetPath()) && !live.count(button->GetPath()))copy->RemoveNameChild(spec);
                else spec->ClearInfo(SdfFieldKeys->CustomData);
            }
            for(UsdPrim button:stage->GetPrimAtPath(panelPath).GetChildren()) {
                // SdfCopySpec preserves authored absolute targets; remap native
                // button pairs with the copied subtree and drop pruned peers.
                if(auto mirror=button.GetRelationship(TfToken("rigExec:picker:mirror"))) {
                    SdfPathVector authored,peers;
                    mirror.GetTargets(&authored);
                    for(const auto &peer:authored) {
                        const auto mapped=peer.ReplacePrefix(templatePath,path);
                        if(mapped.HasPrefix(path) && stage->GetPrimAtPath(mapped))peers.push_back(mapped);
                    }
                    if(peers.empty())button.RemoveProperty(mirror.GetName());
                    else mirror.SetTargets(peers);
                }
                const auto found=live.find(button.GetPath().ReplacePrefix(path,templatePath));
                if(found!=live.end())button.CreateRelationship(TfToken("rigExec:picker:controls"),false).SetTargets(found->second);
            }
        }
        return placed;
    }
    void Pickers() {
        if(!Has(scene,"pickers") || Array(Field(scene,"pickers")).empty())return;
        stage->DefinePrim(SdfPath("/Rig/Pickers"),TfToken("Scope"));
        std::set<std::string> owners;
        std::map<std::string,bool> readOnlyTargets;
        // A translated control is selectable only where standard usdRig
        // interaction can edit it.
        const auto readOnly=[&](Node &control) {
            const auto cached=readOnlyTargets.find(control.id);
            if(cached!=readOnlyTargets.end())return cached->second;
            const bool result=(control.kind=="joint" && control.controlPath==control.path &&
                (Has(control.data,"armature") || HasActiveConstraint(control) ||
                 stage->GetPrimAtPath(control.path).GetAttribute(TfToken("posed:space")).HasAuthoredConnections())) ||
               (control.kind=="control" && HasActiveConstraint(control));
            return readOnlyTargets[control.id]=result;
        };
        int pickerOrder=0;
        for(const auto &data:Array(Field(scene,"pickers"))) {
            auto &owner=Find(Str(data,"owner"));
            if(!owners.insert(owner.id).second)throw std::runtime_error("duplicate picker owner");
            const auto pickerPath=SdfPath("/Rig/Pickers").AppendChild(TfToken(owner.name));
            auto picker=rigExec::RigExecSchemaPrim::Define(stage,pickerPath,TfToken("RigExecPicker"));
            Set(picker,TfToken("ui:label"),VtValue(Str(data,"name")));
            if(pickerOrder)Set(picker,TfToken("ui:order"),VtValue(pickerOrder));
            ++pickerOrder;
            picker.SetRelationship(TfToken("rigExec:picker:rig"),{SdfPath("/Rig")});
            picker.GetPrim().SetCustomDataByKey(TfToken("blender:sourceKind"),VtValue(Str(data,"source_kind")));
            if(Has(data,"source_ui") && !Str(data,"source_ui").empty())
                Warn("picker source UI script is retained as metadata and not executed: "+owner.id);
            // The selectable set is what the source pages offer: a source
            // button with a read-only member selects nothing, never a subset.
            // rank: 0 listed alone, 1 in a group, 2 only through a settings binding.
            struct Target { Node *node; size_t page; int rank; };
            struct Group { size_t page; std::string label; std::vector<Node *> members; };
            std::map<std::string,Target> targets;
            std::vector<Node *> ordered;
            std::vector<Group> groups;
            VtStringArray unavailable,reasons;
            const auto &pages=Array(Field(data,"pages"));
            for(size_t pageIndex=0;pageIndex<pages.size();++pageIndex) {
                const auto pageName=Str(pages[pageIndex],"name");
                for(const auto &buttonData:Array(Field(pages[pageIndex],"buttons"))) {
                    const auto label=Str(buttonData,"label");
                    const bool sourceBinding=Has(buttonData,"source") && Has(Field(buttonData,"source"),"binding");
                    // A binding that selects nothing and states why it is unsupported is a setting.
                    if(sourceBinding)
                        Warn((Array(Field(buttonData,"controls")).empty() && Has(Field(buttonData,"source"),"unsupported_reason") ?
                            "picker setting unavailable; source behavior is not translated: " :
                            "picker binding is selection-only; settings and operators are not translated: ")+label);
                    std::vector<Node *> members;
                    std::set<std::string> unique;
                    bool blocked=false;
                    for(const auto &id:Array(Field(buttonData,"controls"))) {
                        auto &control=Find(String(id));
                        if(!unique.insert(control.id).second)throw std::runtime_error("duplicate picker control target");
                        if(readOnly(control)) {
                            Warn("picker target is read-only in standard usdRig interaction: "+control.id);
                            blocked=true;
                        }
                        else members.push_back(&control);
                    }
                    if(blocked || members.empty()) {
                        // Settings, operators and read-only selections stay as picker metadata, not dead buttons.
                        unavailable.push_back(pageName+": "+label);
                        reasons.push_back(blocked ? "Read-only target in standard usdRig interaction" :
                            sourceBinding && Has(Field(buttonData,"source"),"unsupported_reason") ? Str(Field(buttonData,"source"),"unsupported_reason") :
                            sourceBinding ? "Source settings and operators are not translated" : "No source control");
                        continue;
                    }
                    const int rank=sourceBinding ? 2 : members.size()==1 ? 0 : 1;
                    // A source button that selects several controls keeps its whole set as one group button.
                    if(members.size()>1)groups.push_back({pageIndex,label,members});
                    for(Node *member:members) {
                        const auto inserted=targets.emplace(member->id,Target{member,pageIndex,rank});
                        if(inserted.second)ordered.push_back(member);
                        else if(rank<inserted.first->second.rank)inserted.first->second=Target{member,pageIndex,rank};
                    }
                }
            }
            const auto placed=BipedPicker(owner,pickerPath,ordered);
            std::set<std::string> labels;
            for(const auto &panel:picker.GetPrim().GetChildren()) {
                std::string label;
                if(panel.GetAttribute(TfToken("ui:label")).Get(&label))labels.insert(label);
            }
            // Every remaining target gets exactly one button, on the first
            // source page that lists it alone, else in a group, else through
            // a settings binding. Pages are cut by count after the placed
            // controls are gone; a page's group buttons come first.
            int panelIndex=0,overflow=0;
            size_t group=0;
            for(size_t pageIndex=0;pageIndex<pages.size();++pageIndex) {
                std::vector<std::pair<std::string,SdfPathVector>> rest;
                for(;group<groups.size() && groups[group].page==pageIndex;++group) {
                    SdfPathVector members;
                    for(const Node *member:groups[group].members)members.push_back(member->controlPath);
                    rest.emplace_back(groups[group].label,std::move(members));
                }
                const size_t pageGroups=rest.size();
                std::set<std::string> seen;
                for(const auto &buttonData:Array(Field(pages[pageIndex],"buttons")))
                    for(const auto &id:Array(Field(buttonData,"controls"))) {
                        const auto found=targets.find(String(id));
                        if(found!=targets.end() && found->second.page==pageIndex &&
                           !placed.count(found->second.node->controlPath) && seen.insert(found->first).second)
                            rest.emplace_back(Str(found->second.node->data,"name"),SdfPathVector{found->second.node->controlPath});
                    }
                // A Rigify title such as "(Tweak)" only reads next to its
                // parent row; the collection name stands alone. A page named
                // like a biped panel holds what that panel has no slot for.
                auto pageLabel=Str(pages[pageIndex],"name");
                if(!pageLabel.empty() && pageLabel[0]=='(' && Has(pages[pageIndex],"source")) {
                    const auto &origin=Field(pages[pageIndex],"source");
                    if(origin.IsObject() && Has(origin,"collection") && Field(origin,"collection").IsString())pageLabel=Str(origin,"collection");
                }
                if(labels.count(pageLabel))pageLabel+=" (more)";
                const size_t chunks=(rest.size()+47)/48;
                for(size_t chunk=0;chunk<chunks;++chunk) {
                    const size_t count=std::min<size_t>(48,rest.size()-chunk*48);
                    auto panel=rigExec::RigExecSchemaPrim::Define(stage,pickerPath.AppendChild(TfToken("page_"+std::to_string(panelIndex))),TfToken("RigExecPickerPanel"));
                    Set(panel,TfToken("ui:label"),VtValue(pageLabel+
                        (chunks>1 ? " "+std::to_string(chunk+1)+"/"+std::to_string(chunks) : std::string())));
                    Set(panel,TfToken("ui:order"),VtValue(2+panelIndex++));
                    Set(panel,TfToken("ui:size"),VtValue(GfVec2f(528,std::max(90.0f,30.0f*float((count+2)/3)))));
                    for(size_t index=0;index<count;++index) {
                        const auto &entry=rest[chunk*48+index];
                        const bool grouped=chunk*48+index<pageGroups;
                        overflow+=!grouped;
                        auto button=rigExec::RigExecSchemaPrim::Define(stage,panel.GetPath().AppendChild(TfToken("button_"+std::to_string(index))),TfToken("RigExecPickerButton"));
                        if(index)Set(button,TfToken("ui:position"),VtValue(GfVec2f(float(176*(index%3)),float(30*(index/3)))));
                        Set(button,TfToken("ui:size"),VtValue(GfVec2f(168,24)));
                        Set(button,TfToken("ui:shape"),VtValue(TfToken("roundedRectangle")));
                        Set(button,TfToken("ui:roundness"),VtValue(0.15f));
                        Set(button,TfToken("ui:fill"),VtValue(grouped ? GfVec4f(0.42f,0.30f,0.52f,1) : GfVec4f(0.18f,0.32f,0.48f,1)));
                        Set(button,TfToken("ui:textColor"),VtValue(GfVec4f(1)));
                        Set(button,TfToken("ui:text"),VtValue(entry.first));
                        Set(button,TfToken("ui:fontSize"),VtValue(10.0f));
                        button.SetRelationship(TfToken("rigExec:picker:controls"),entry.second);
                    }
                }
            }
            auto prim=picker.GetPrim();
            prim.SetCustomDataByKey(TfToken("blender:controlCount"),VtValue(int(ordered.size())));
            prim.SetCustomDataByKey(TfToken("blender:bipedCount"),VtValue(int(placed.size())));
            prim.SetCustomDataByKey(TfToken("blender:overflowCount"),VtValue(overflow));
            prim.SetCustomDataByKey(TfToken("blender:groupCount"),VtValue(int(groups.size())));
            if(!unavailable.empty()) {
                prim.SetCustomDataByKey(TfToken("blender:unavailable"),VtValue(unavailable));
                prim.SetCustomDataByKey(TfToken("blender:unsupportedReason"),VtValue(reasons));
            }
        }
    }
    // The probe reads a few leg joints, so it compiles only what those can
    // depend on. The closure is taken from the authored layer alone: namespace
    // ancestors, every connection source and relationship target of a kept
    // prim, and every prim holding a relationship to a kept prim that may
    // write it. Only relationships known to read their targets are exempt, so
    // an unknown one grows the copy instead of dropping a dependency. Sibling
    // order is preserved. Null means no closure, for the reason in why; the
    // caller then probes the rig.
    UsdStageRefPtr CalibrationStage(const SdfPathVector &seeds,std::string &why) {
        static const std::set<std::string> reads{"rigExec:poseInputs","rigExec:parent","rigExec:source","rigExec:sources",
            "rigExec:sourceObject","rigExec:sourceParent","rigExec:ownerObject","rigExec:ownerParent","rigExec:customSpace",
            "rigExec:targets","rigExec:targetObjects","rigExec:poleVectorObjects","rigExec:channelSpaces",
            "rigExec:influences","rigExec:owner","rigExec:transform"};
        const auto layer=stage->GetRootLayer();
        const SdfPath root("/Rig"),top=SdfPath::AbsoluteRootPath();
        TfErrorMark mark;
        std::map<SdfPath,SdfPathVector> writers;
        std::function<void(const SdfPrimSpecHandle &)> index=[&](const SdfPrimSpecHandle &prim) {
            for(const auto &relationship:prim->GetRelationships()) {
                const auto &name=relationship->GetName();
                if(!TfStringStartsWith(name,"rigExec:") || TfStringStartsWith(name,"rigExec:picker:") || reads.count(name)) continue;
                for(const auto &target:relationship->GetTargetPathList().GetAddedOrExplicitItems())
                    writers[target.GetPrimPath()].push_back(prim->GetPath());
            }
            for(const auto &child:prim->GetNameChildren()) index(child);
        };
        if(const auto prim=layer->GetPrimAtPath(root)) index(prim); else { why="no /Rig prim"; return nullptr; }
        std::set<SdfPath> kept; SdfPathVector todo=seeds;
        while(!todo.empty()) {
            const auto path=todo.back(); todo.pop_back();
            if(path==top || kept.count(path)) continue;
            const auto prim=layer->GetPrimAtPath(path);
            // A dangling target is dangling in the whole rig as well.
            if(!prim) continue;
            if(!path.HasPrefix(root)) { why="dependency outside /Rig: "+path.GetString(); return nullptr; }
            kept.insert(path); todo.push_back(path.GetParentPath());
            for(const auto &attribute:prim->GetAttributes())
                for(const auto &target:attribute->GetConnectionPathList().GetAddedOrExplicitItems()) todo.push_back(target.GetPrimPath());
            for(const auto &relationship:prim->GetRelationships())
                for(const auto &target:relationship->GetTargetPathList().GetAddedOrExplicitItems()) todo.push_back(target.GetPrimPath());
            const auto writer=writers.find(path);
            if(writer!=writers.end()) todo.insert(todo.end(),writer->second.begin(),writer->second.end());
        }
        for(const auto &seed:seeds) if(!kept.count(seed)) { why="probed prim is not authored: "+seed.GetString(); return nullptr; }
        auto copy=SdfLayer::CreateAnonymous("calibration.usda");
        for(const auto &field:layer->ListFields(top))
            if(field!=SdfChildrenKeys->PrimChildren) copy->SetField(top,field,layer->GetField(top,field));
        const auto values=[](SdfSpecType,const TfToken &,const SdfLayerHandle &,const SdfPath &,bool,
            const SdfLayerHandle &,const SdfPath &,bool,std::optional<VtValue> *) { return true; };
        const auto children=[&](const TfToken &field,const SdfLayerHandle &,const SdfPath &path,bool authored,
            const SdfLayerHandle &,const SdfPath &,bool,std::optional<VtValue> *from,std::optional<VtValue> *to) {
            if(field!=SdfChildrenKeys->PrimChildren || !authored) return true;
            TfTokenVector names;
            for(const auto &name:layer->GetFieldAs<TfTokenVector>(path,field)) if(kept.count(path.AppendChild(name))) names.push_back(name);
            *from=VtValue(names); *to=VtValue(names);
            return !names.empty();
        };
        UsdStageRefPtr result;
        if(SdfCreatePrimInLayer(copy,root) && SdfCopySpec(layer,root,copy,root,values,children)) result=UsdStage::Open(copy);
        if(!mark.IsClean()) { why=mark.GetBegin()->GetCommentary(); mark.Clear(); return nullptr; }
        if(!result) { why="closure copy failed"; return nullptr; }
        // RigExec does not tolerate a target that the copy lost.
        for(const auto &path:kept) if(!result->GetPrimAtPath(path)) { why="closure copy lost "+path.GetString(); return nullptr; }
        return result;
    }
    void CalibrateInertLegChains() {
        struct Candidate { Node *owner; Node *source; std::string side; Node *control; };
        const auto &items=Array(Field(scene,"constraints"));
        std::vector<Candidate> candidates;
        for(const auto &entry:ActiveStacks()) {
            auto &owner=Find(entry.first);
            if(owner.kind!="joint" || !Flag(owner.data,"connected",false))continue;
            // Rigify limbs.leg and limbs.paw name the ORG-, DEF- and MCH- bones
            // of a leg after the metarig's thigh, shin, foot and toe (r_toe on
            // the bundled animal metarigs), further segments numbered .001.
            static const std::set<std::string> legBones{"thigh","shin","foot","toe","r_toe"};
            auto part=PickerSide(Str(owner.data,"name"));
            const auto dot=part.second.rfind('.');
            if(dot!=std::string::npos && dot+1<part.second.size() &&
               part.second.find_first_not_of("0123456789",dot+1)==std::string::npos)part.second.erase(dot);
            if(part.first=='M' || part.second.size()<5 || !legBones.count(part.second.substr(4)) ||
               (part.second.compare(0,4,"org-") && part.second.compare(0,4,"def-") && part.second.compare(0,4,"mch-")))continue;
            const std::string side(1,part.first);
            const auto &first=items[entry.second.front()];
            if(Str(first,"type")!="COPY_TRANSFORMS" || Number(Field(first,"influence"))!=1 ||
               Str(first,"owner_space")!="WORLD" || Str(first,"target_space")!="WORLD")continue;
            bool onlyCopies=std::all_of(entry.second.begin(),entry.second.end(),[&](size_t c){return Str(items[c],"type")=="COPY_TRANSFORMS";});
            if(!std::all_of(entry.second.begin(),entry.second.end(),[&](size_t c){
                    const auto type=Str(items[c],"type");return type=="COPY_TRANSFORMS" || type=="STRETCH_TO";
                }))continue;
            auto &source=Find(Str(onlyCopies ? items[entry.second.back()] : first,"source"));
            if(source.kind=="joint")candidates.push_back({&owner,&source,side,nullptr});
        }
        if(candidates.empty())return;
        SdfPathVector seeds;
        for(auto &candidate:candidates) {
            seeds.push_back(candidate.owner->path); seeds.push_back(candidate.source->path);
            if(Str(candidate.owner->data,"name")!="ORG-shin."+candidate.side)continue;
            const auto armature=Str(candidate.owner->data,"armature");
            auto control=std::find_if(nodes.begin(),nodes.end(),[&](const auto &entry) {
                const auto &node=entry.second;
                return node.kind=="joint" && Has(node.data,"armature") &&
                    Str(node.data,"armature")==armature && Str(node.data,"name")=="foot_ik."+candidate.side;
            });
            if(control==nodes.end())continue;
            candidate.control=&control->second;
            seeds.push_back(control->second.path); seeds.push_back(control->second.controlPath);
        }
        UsdStageRefPtr probe;
        std::unique_ptr<rigExec::RigExecRigEvaluator> compiled;
        rigExec::RigExecRigPose baseline;
        std::string failure;
        auto frame=[&](const rigExec::RigExecRigPose &pose,const Node &node)->const rigExec::RigExecPointFrame * {
            auto it=pose.jointFramesFinal.find(node.path);
            return it==pose.jointFramesFinal.end()?nullptr:&it->second;
        };
        auto open=[&](const UsdStageRefPtr &target,bool complete) {
            if(!target)return false;
            probe=target; compiled=std::make_unique<rigExec::RigExecRigEvaluator>(probe,SdfPath("/Rig"));
            std::vector<std::string> errors;
            if(!compiled->Compile(&errors)) { failure=errors.empty()?std::string("native evaluation failed"):errors.front();return false; }
            baseline=compiled->Evaluate(UsdTimeCode::Default());
            if(!baseline.valid) { failure="invalid saved pose";return false; }
            // The closure must pose every probed joint, or it proves nothing.
            if(complete && !std::all_of(candidates.begin(),candidates.end(),[&](const Candidate &candidate) {
                return frame(baseline,*candidate.owner) && frame(baseline,*candidate.source);
            })) { failure="closure does not pose every probed joint";return false; }
            return true;
        };
        // probeWholeRig=1 probes the whole rig, to verify the closure against
        // it. That path compiles everything and is slow on a production rig,
        // so taking it is always diagnosed.
        failure=wholeProbe?"probeWholeRig=1":"";
        if(wholeProbe || !open(CalibrationStage(seeds,failure),true)) {
            Warn("connected lower-limb calibration probed the whole rig: "+failure);
            if(!open(stage,false)) { Warn("connected lower-limb calibration skipped: "+failure);return; }
        }
        auto &evaluator=*compiled;
        auto matrix=[](const rigExec::RigExecPointFrame &frame) {
            GfMatrix4d result(1);const auto origin=frame.Origin();
            const GfVec3d axes[3]={frame.X()-origin,frame.Y()-origin,frame.Z()-origin};
            for(int i=0;i<3;++i)for(int j=0;j<3;++j)result[i][j]=axes[i][j];
            result.SetTranslateOnly(origin);return result;
        };
        std::set<std::pair<std::string,std::string>> inertSides;
        for(const auto &candidate:candidates) {
            if(!candidate.control)continue;
            const auto armature=Str(candidate.owner->data,"armature");
            auto attr=probe->GetPrimAtPath(candidate.control->controlPath).GetAttribute(TfToken("avars:tx"));
            double saved=0;
            if(!attr || !attr.Get(&saved))continue;
            // The nudge and both thresholds are relative to the shin, so the
            // verdict does not depend on scene units or character size.
            const double reach=std::max(Num(candidate.owner->data,"length",1),1e-9);
            probe->SetEditTarget(probe->GetSessionLayer());
            attr.Set(saved+0.12*reach);
            const auto moved=evaluator.Evaluate(UsdTimeCode::Default());
            probe->GetSessionLayer()->Clear();
            probe->SetEditTarget(probe->GetRootLayer());
            if(!moved.valid)continue;
            const auto *owner0=frame(baseline,*candidate.owner),*owner1=frame(moved,*candidate.owner);
            const auto *source0=frame(baseline,*candidate.source),*source1=frame(moved,*candidate.source);
            if(!owner0 || !owner1 || !source0 || !source1)continue;
            auto distance=[&](const GfMatrix4d &a,const GfMatrix4d &b) {
                double result=0;for(int i=0;i<4;++i)for(int j=0;j<3;++j)
                    result=std::max(result,std::abs(a[i][j]-b[i][j])/(i<3 ? 1 : reach));return result;
            };
            if(distance(matrix(*owner0),matrix(*owner1))<1e-5 &&
               distance(matrix(*source0),matrix(*source1))>1e-4)
                inertSides.insert({armature,candidate.side});
        }
        for(const auto &candidate:candidates) {
            if(!inertSides.count({Str(candidate.owner->data,"armature"),candidate.side}))continue;
            const auto *target=frame(baseline,*candidate.owner),*source=frame(baseline,*candidate.source);
            if(!target || !source)continue;
            auto mapped=rigExec::RigExecSchemaPrim::Define(stage,SdfPath("/Rig/Mechanisms").AppendChild(
                TfToken("calibrated_leg_"+candidate.owner->name)),TfToken("RigExecMappedFrame"));
            Set(mapped,TfToken("inputs:targetRest"),VtValue(matrix(*target)));
            Set(mapped,TfToken("inputs:sourceRest"),VtValue(matrix(*source)));
            mapped.SetRelationship(TfToken("rigExec:source"),{candidate.source->path});
            mapped.SetRelationship(TfToken("rigExec:poseInputs"),{candidate.source->path});
            stage->GetPrimAtPath(candidate.owner->path).GetAttribute(TfToken("posed:space")).SetConnections({
                mapped.GetPath().AppendProperty(TfToken("outputs:matrix"))});
        }
        if(!inertSides.empty())
            Warn("connected lower-limb IK chains use calibrated native frames; Stretch To length/volume remains approximate");
    }
    // On every evaluation, however little changed, usdRig queries the
    // namespace between each provider and its nearest ancestor provider. The
    // scopes that only collect providers are therefore one static, hidden
    // provider each. Their providers are absolute or switched frames, so its
    // channels cancel out of them; /Rig/Dag stays a scope because its roots
    // would inherit. Its rest does not cancel: the skin providers bind
    // through it. So, like a Control, it authors its default space, which
    // refuses stock Pivot editing.
    void Holders() {
        for(const char *name:{"Controls","ControlSpaces","TranslationSpaces","Mechanisms"}) {
            const auto scope=stage->GetPrimAtPath(SdfPath("/Rig").AppendChild(TfToken(name)));
            if(!scope) continue;
            const auto range=scope.GetDescendants();
            if(std::none_of(range.begin(),range.end(),[](const UsdPrim &prim) {
                return prim.GetTypeName()=="RigExecControl" || prim.GetTypeName()=="RigExecJoint"; })) continue;
            const SdfPath identity=IdentitySource();
            scope.SetTypeName(TfToken("RigExecControl"));
            const auto holder=rigExec::RigExecSchemaPrim::Get(stage,scope.GetPath(),TfToken("RigExecControl"));
            Set(holder,TfToken("guide:displayOpacity"),VtValue(0.0f));
            holder.GetPrim().GetAttribute(TfToken("default:space")).SetConnections({identity});
            // One extent carries one purpose: the animator Controls are guides.
            if(std::string(name)=="Controls") UsdGeomImageable(scope).CreatePurposeAttr().Set(UsdGeomTokens->guide);
        }
    }
    SdfLayerRefPtr Run(bool metadataOnly,bool strict,bool sourceData,bool pruneUnobserved,bool probeWholeRig) {
        prune=pruneUnobserved; wholeProbe=probeWholeRig;
        if(Str(scene,"format")!="usdBlenderRig" || Integer(Field(scene,"version"))!=1)
            throw std::runtime_error("unsupported Blender rig snapshot format/version");
        const double units=Number(Field(scene,"meters_per_unit")),fps=Number(Field(scene,"fps"));
        if(units<=0 || fps<=0) throw std::runtime_error("invalid units/frame rate");
        UsdGeomSetStageUpAxis(stage,UsdGeomTokens->z); UsdGeomSetStageMetersPerUnit(stage,units);
        stage->SetTimeCodesPerSecond(fps); stage->SetFramesPerSecond(fps);
        stage->SetStartTimeCode(Number(Field(scene,"start"))); stage->SetEndTimeCode(Number(Field(scene,"end")));
        for(const auto &v:Array(Field(scene,"diagnostics"))) Warn(String(v));
        for(const auto &v:Array(Field(scene,"dependencies"))) Asset(String(v));
        if(!metadataOnly) { Graph(); Constraints(); Guides(); Materials(); Lattices(); Geometry(); Pickers(); Holders(); }
        std::reverse(operationOrder.begin(),operationOrder.end());
        if(!operationOrder.empty()) stage->GetPrimAtPath(SdfPath("/Rig/Movers")).SetChildrenReorder(operationOrder);
        auto root=stage->GetPrimAtPath(SdfPath("/Rig"));
        stage->SetDefaultPrim(root);
        if(!metadataOnly) CalibrateInertLegChains();
        std::sort(diagnostics.begin(),diagnostics.end()); diagnostics.erase(std::unique(diagnostics.begin(),diagnostics.end()),diagnostics.end());
        std::sort(dependencies.begin(),dependencies.end()); dependencies.erase(std::unique(dependencies.begin(),dependencies.end()),dependencies.end());
        if(strict && !diagnostics.empty()) throw std::runtime_error("strict import rejected: "+diagnostics.front());
        root.CreateAttribute(TfToken("blender:diagnostics"),SdfValueTypeNames->StringArray,true,SdfVariabilityUniform)
            .Set(VtStringArray(diagnostics.begin(),diagnostics.end()));
        // The snapshot is inert for evaluation and several times the rig's
        // size, so the JSON dump is an explicit request.
        if(sourceData) root.SetCustomDataByKey(TfToken("blender:sourceData"),VtValue(JsWriteToString(scene)));
        auto layer=stage->GetRootLayer();
        layer->SetCustomLayerData({{"blenderRig:source",VtValue(source)},
            {"blenderRig:diagnostics",VtValue(VtStringArray(diagnostics.begin(),diagnostics.end()))},
            {"blenderRig:dependencies",VtValue(VtStringArray(dependencies.begin(),dependencies.end()))},
            {"blenderRig:complete",VtValue(!metadataOnly && diagnostics.empty())}});
        return layer;
    }
};
}
SdfLayerRefPtr Translate(const std::string &json,const std::string &source,bool metadataOnly,bool strict,bool sourceData,bool pruneUnobserved,bool probeWholeRig) {
    if(json.size()>512*1024*1024) throw std::runtime_error("snapshot exceeds 512 MiB");
    JsParseError error; auto scene=JsParseString(json,&error);
    if(!scene.IsObject()) throw std::runtime_error("invalid snapshot JSON at line "+std::to_string(error.line)+": "+error.reason);
    return Importer(std::move(scene),source).Run(metadataOnly,strict,sourceData,pruneUnobserved,probeWholeRig);
}
}
