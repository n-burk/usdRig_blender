// Declarative Exec computations; all pose inputs are USD attributes/providers.
// Coordinate/numerical contracts and primary sources: docs/references.md.
#include "rigExec/types.h"
#include "rigExecMath/dualQuat.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/exec/exec/builtinComputations.h"
#include "pxr/exec/exec/registerSchema.h"
#include "pxr/exec/vdf/context.h"
#include "pxr/exec/vdf/readIterator.h"
#include "pxr/base/vt/array.h"
#include <algorithm>
#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE
TF_DEFINE_PRIVATE_TOKENS(_armature,
    ((matrix,"outputs:matrix")) ((local,"inputs:local"))
    ((inverseBind,"inputs:inverseBind")) ((parent,"rigExec:parent"))
    ((source,"rigExec:source")) ((sourceObject,"rigExec:sourceObject"))
    ((incoming,"inputs:incoming")) ((useIncoming,"inputs:useIncoming"))
    ((preserveLocation,"inputs:preserveLocation"))
    ((tx,"inputs:tx")) ((ty,"inputs:ty")) ((tz,"inputs:tz"))
    ((rx,"inputs:rx")) ((ry,"inputs:ry")) ((rz,"inputs:rz"))
    ((sx,"inputs:sx")) ((sy,"inputs:sy")) ((sz,"inputs:sz"))
    (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_bone,
    ((matrix,"outputs:matrix")) ((local,"inputs:local"))
    ((parentRest,"inputs:parentRest")) ((hasParent,"inputs:hasParent"))
    ((inheritRotation,"inputs:inheritRotation")) ((inheritScale,"inputs:inheritScale"))
    ((localLocation,"inputs:localLocation")) ((connected,"inputs:connected"))
    ((parent,"rigExec:parent")) ((object,"rigExec:sourceObject"))
    ((tx,"inputs:tx")) ((ty,"inputs:ty")) ((tz,"inputs:tz"))
    ((rx,"inputs:rx")) ((ry,"inputs:ry")) ((rz,"inputs:rz"))
    ((sx,"inputs:sx")) ((sy,"inputs:sy")) ((sz,"inputs:sz")) (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_skin,
    ((matrix,"outputs:matrix")) ((inverseBind,"inputs:inverseBind"))
    ((inverseMesh,"inputs:inverseMesh")) ((fromBind,"inputs:fromBind"))
    ((followOnly,"inputs:followOnly"))
    ((owner,"rigExec:owner")) ((source,"rigExec:source"))
    ((sourceObject,"rigExec:sourceObject")) (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_constraint,
    ((matrix,"outputs:matrix")) ((incoming,"inputs:incoming")) ((origin,"inputs:origin"))
    ((inverseBind,"inputs:inverseBind")) ((operation,"inputs:operation"))
    ((influence,"inputs:influence"))
    ((targetIndices,"inputs:targetIndices")) ((objectIndices,"inputs:objectIndices"))
    ((targetBinds,"inputs:targetBinds")) ((targetWeights,"inputs:targetWeights"))
    ((pivot,"inputs:pivot")) ((dualQuaternion,"inputs:dualQuaternion")) ((currentPivot,"inputs:currentPivot"))
    ((targets,"rigExec:targets")) ((targetObjects,"rigExec:targetObjects"))
    ((ownerSpace,"inputs:ownerSpace")) ((targetSpace,"inputs:targetSpace"))
    ((axisMask,"inputs:axisMask")) ((invertMask,"inputs:invertMask")) ((offset,"inputs:offset"))
    ((uniformScale,"inputs:uniformScale")) ((scaleAdd,"inputs:scaleAdd")) ((power,"inputs:power"))
    ((rotationMix,"inputs:rotationMix")) ((removeTargetShear,"inputs:removeTargetShear"))
    ((trackAxis,"inputs:trackAxis")) ((keepAxis,"inputs:keepAxis")) ((volume,"inputs:volume"))
    ((restLength,"inputs:restLength")) ((bulge,"inputs:bulge"))
    ((bulgeMin,"inputs:bulgeMin")) ((bulgeMax,"inputs:bulgeMax")) ((bulgeSmooth,"inputs:bulgeSmooth"))
    ((useBulgeMin,"inputs:useBulgeMin")) ((useBulgeMax,"inputs:useBulgeMax"))
    ((targetOffset,"inputs:targetOffset")) ((source,"rigExec:source"))
    ((sourceObject,"rigExec:sourceObject")) ((ownerObject,"rigExec:ownerObject"))
    ((customSpace,"rigExec:customSpace")) (computePointFrame)
);
TF_DEFINE_PRIVATE_TOKENS(_localSpaces,
    ((ownerLocal,"inputs:ownerLocal")) ((ownerRest,"inputs:ownerRest")) ((ownerParentRest,"inputs:ownerParentRest"))
    ((ownerHasParent,"inputs:ownerHasParent")) ((ownerInheritRotation,"inputs:ownerInheritRotation"))
    ((ownerLocalLocation,"inputs:ownerLocalLocation")) ((ownerInheritScale,"inputs:ownerInheritScale"))
    ((sourceLocal,"inputs:sourceLocal")) ((sourceRest,"inputs:sourceRest")) ((sourceParentRest,"inputs:sourceParentRest"))
    ((sourceHasParent,"inputs:sourceHasParent")) ((sourceInheritRotation,"inputs:sourceInheritRotation"))
    ((sourceLocalLocation,"inputs:sourceLocalLocation")) ((sourceInheritScale,"inputs:sourceInheritScale"))
    ((ownerParent,"rigExec:ownerParent")) ((sourceParent,"rigExec:sourceParent"))
);
namespace {
GfMatrix4d FrameMatrix(const rigExec::RigExecPointFrame *frame) {
    GfMatrix4d result(1);
    if (!frame) return result;
    auto origin=frame->Origin();
    GfVec3d axes[3]={frame->X()-origin,frame->Y()-origin,frame->Z()-origin};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) result[i][j]=axes[i][j];
    result.SetTranslateOnly(origin);
    return result;
}
GfMatrix4d Channels(const VdfContext &ctx) {
    GfMatrix4d local(1);
    local.SetScale(GfVec3d(ctx.GetInputValue<double>(_armature->sx),
                         ctx.GetInputValue<double>(_armature->sy),ctx.GetInputValue<double>(_armature->sz)));
    const TfToken rotations[]={_armature->rx,_armature->ry,_armature->rz};
    for(int i=0;i<3;++i) {
        GfVec3d axis(0);axis[i]=1;
        local*=GfMatrix4d(GfRotation(axis,ctx.GetInputValue<double>(rotations[i])),GfVec3d(0));
    }
    local.SetTranslateOnly(GfVec3d(ctx.GetInputValue<double>(_armature->tx),
                                 ctx.GetInputValue<double>(_armature->ty),ctx.GetInputValue<double>(_armature->tz)));
    return local;
}
GfMatrix4d ArmatureParent(const VdfContext &ctx) {
    GfMatrix4d incoming = ctx.GetInputValue<bool>(_armature->useIncoming)
        ? ctx.GetInputValue<GfMatrix4d>(_armature->incoming)
        : Channels(ctx)*ctx.GetInputValue<GfMatrix4d>(_armature->local)*
          FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_armature->parent));
    // Blender's Armature constraint applies the target's rest-to-pose map
    // over the incoming owner frame, preserving the owner's local channels.
    auto result=incoming*
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_armature->sourceObject)).GetInverse()*
        ctx.GetInputValue<GfMatrix4d>(_armature->inverseBind)*
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_armature->source));
    if(ctx.GetInputValue<bool>(_armature->preserveLocation)) result.SetTranslateOnly(incoming.ExtractTranslation());
    return result;
}
GfVec3d Row(const GfMatrix4d &m,int i) {return GfVec3d(m[i][0],m[i][1],m[i][2]);}
void SetRow(GfMatrix4d &m,int i,const GfVec3d &v) {for(int j=0;j<3;++j)m[i][j]=v[j];}
GfVec3d Sizes(const GfMatrix4d &m) {return GfVec3d(Row(m,0).GetLength(),Row(m,1).GetLength(),Row(m,2).GetLength());}
GfVec3d VolumeSizes(const GfMatrix4d &m) {
    auto sizes=Sizes(m);double product=sizes[0]*sizes[1]*sizes[2];
    return product>0 ? sizes*std::cbrt(std::abs(m.GetDeterminant())/product) : sizes;
}
void Rescale(GfMatrix4d &m,const GfVec3d &s) {for(int i=0;i<3;++i)SetRow(m,i,Row(m,i)*s[i]);}
void NormalizeRows(GfMatrix4d &m) {for(int i=0;i<3;++i){auto v=Row(m,i);v.Normalize();SetRow(m,i,v);}}
// Symmetric orthogonalization around the bone's Y axis: project X/Z, then
// split their angular error equally. Preserving area retains volume under
// shear. This is deliberately different from a generic polar matrix split.
void Orthogonalize(GfMatrix4d &m,bool normalize) {
    auto y=Row(m,1),x=Row(m,0),z=Row(m,2);
    double y2=y.GetLengthSq();
    if(y2>0) {x-=y*(GfDot(x,y)/y2);z-=y*(GfDot(z,y)/y2);if(normalize)y/=std::sqrt(y2);}
    double lx=x.Normalize(),lz=z.Normalize();
    double cosine=std::clamp(GfDot(x,z),-1.0,1.0);
    if(std::abs(cosine)>1e-4 && std::abs(cosine)<1-1.1920928955078125e-7) {
        auto sum=x+z,difference=x-z;sum.Normalize();difference.Normalize();
        x=(sum+difference)/std::sqrt(2.0);z=(sum-difference)/std::sqrt(2.0);
        double area=std::sqrt(std::sqrt(std::max(0.0,1-cosine*cosine)));
        lx*=area;lz*=area;
    }
    SetRow(m,0,x*(normalize?1:lx));SetRow(m,1,y);SetRow(m,2,z*(normalize?1:lz));
}
struct ParentSpaces {
    GfMatrix4d rotationScale,location;
    GfVec3d post;
    GfMatrix4d Apply(const GfMatrix4d &input,bool inverse=false) const {
        auto rotation=inverse?rotationScale.GetInverse():rotationScale;
        auto position=inverse?location.GetInverse():location;
        auto output=input*rotation;
        output.SetTranslateOnly(position.Transform(input.ExtractTranslation()));
        Rescale(output,inverse?GfVec3d(1/post[0],1/post[1],1/post[2]):post);
        return output;
    }
};
ParentSpaces BoneParentSpaces(const GfMatrix4d &rest,const GfMatrix4d &parentRest,
    const GfMatrix4d &parent,bool hasParent,const std::string &mode,bool inheritRotation,bool localLocation) {
    GfMatrix4d rotationScale=rest,location=rest;GfVec3d post(1);
    if(hasParent) {
        GfMatrix4d adjusted=parent;
        if(inheritRotation) {
            if(mode=="NONE" || mode=="AVERAGE") Orthogonalize(adjusted,true);
            else if(mode=="ALIGNED") {Orthogonalize(adjusted,false);post=Sizes(adjusted);NormalizeRows(adjusted);}
            else if(mode=="NONE_LEGACY") NormalizeRows(adjusted);
        } else {
            adjusted=parentRest;
            if(mode=="FULL") Rescale(adjusted,Sizes(parent));
            else if(mode=="FIX_SHEAR" || mode=="ALIGNED") {
                if(mode=="ALIGNED") post=VolumeSizes(parent);else Rescale(adjusted,VolumeSizes(parent));
            }
        }
        if(mode=="AVERAGE") Rescale(adjusted,GfVec3d(std::cbrt(std::abs(parent.GetDeterminant()))));
        rotationScale=rest*adjusted;
        if(mode=="FIX_SHEAR") Orthogonalize(rotationScale,false);
        location=rest*parent;
        if(!localLocation) {location=parent;location.SetTranslateOnly(parent.Transform(rest.ExtractTranslation()));}
    } else if(!localLocation) location.SetTranslate(rest.ExtractTranslation());
    return {rotationScale,location,post};
}
GfMatrix4d BoneFrame(const VdfContext &ctx) {
    auto object=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_bone->object));
    auto channels=Channels(ctx);
    if(ctx.GetInputValue<bool>(_bone->connected)) channels.SetTranslateOnly(GfVec3d(0));
    auto spaces=BoneParentSpaces(ctx.GetInputValue<GfMatrix4d>(_bone->local),
        ctx.GetInputValue<GfMatrix4d>(_bone->parentRest),
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_bone->parent))*object.GetInverse(),
        ctx.GetInputValue<bool>(_bone->hasParent),ctx.GetInputValue<TfToken>(_bone->inheritScale).GetString(),
        ctx.GetInputValue<bool>(_bone->inheritRotation),ctx.GetInputValue<bool>(_bone->localLocation));
    return spaces.Apply(channels)*object;
}
ParentSpaces ConstraintParentSpaces(const VdfContext &ctx,bool source,const GfMatrix4d &object) {
    return BoneParentSpaces(ctx.GetInputValue<GfMatrix4d>(source?_localSpaces->sourceLocal:_localSpaces->ownerLocal),
        ctx.GetInputValue<GfMatrix4d>(source?_localSpaces->sourceParentRest:_localSpaces->ownerParentRest),
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(source?_localSpaces->sourceParent:_localSpaces->ownerParent))*object.GetInverse(),
        ctx.GetInputValue<bool>(source?_localSpaces->sourceHasParent:_localSpaces->ownerHasParent),
        ctx.GetInputValue<TfToken>(source?_localSpaces->sourceInheritScale:_localSpaces->ownerInheritScale).GetString(),
        ctx.GetInputValue<bool>(source?_localSpaces->sourceInheritRotation:_localSpaces->ownerInheritRotation),
        ctx.GetInputValue<bool>(source?_localSpaces->sourceLocalLocation:_localSpaces->ownerLocalLocation));
}
GfMatrix4d SkinInfluence(const VdfContext &ctx) {
    GfMatrix4d prefix(1);
    if(ctx.GetInputValue<bool>(_skin->fromBind))
        prefix=ctx.GetInputValue<GfMatrix4d>(_skin->inverseMesh)*
            FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_skin->owner));
    if(ctx.GetInputValue<bool>(_skin->followOnly)) return prefix;
    return prefix*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_skin->sourceObject)).GetInverse()*
        ctx.GetInputValue<GfMatrix4d>(_skin->inverseBind)*
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_skin->source));
}
GfMatrix4d CopyTransforms(const VdfContext &ctx) {
    auto result=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_armature->source));
    if(ctx.GetInputValue<bool>(_armature->preserveLocation))
        result.SetTranslateOnly(ctx.GetInputValue<GfMatrix4d>(_armature->incoming).ExtractTranslation());
    return result;
}
void Track(GfMatrix4d &m,GfVec3d direction,const std::string &axisToken) {
    // Blender's tracking direction and axis-angle are float values. Retain
    // that boundary: near a half-turn, rounding after the cross product
    // instead of before it changes the chosen rotation axis appreciably.
    GfVec3f targetDirection(direction);
    if(targetDirection.Normalize()==0) return;
    int axis=axisToken.back()-'X';bool negative=axisToken.find("NEGATIVE")!=std::string::npos;
    GfVec3f original(Row(m,axis)*(negative?-1:1));
    if(original.Normalize()==0) {original=GfVec3f(0);original[axis]=negative?-1:1;}
    GfVec3f perpendicular(GfCross(GfVec3d(original),GfVec3d(targetDirection)));
    float sine=perpendicular.Normalize();
    float cosine=std::clamp(GfDot(original,targetDirection),-1.0f,1.0f);
    float angle=std::acos(cosine);
    if(sine<1.1920928955078125e-7) {
        if(angle<3.14159265358979323846-0.01) return;
        // At a half-turn the next local track direction resolves the ambiguity.
        auto next=Row(m,(axis+1)%3)*(axis==2?(negative?1:-1):(negative?-1:1));
        perpendicular=GfVec3f(GfCross(GfVec3d(original),next));if(perpendicular.Normalize()==0)return;
        angle=3.14159265358979323846;
    } else if(sine<0.1f) angle=cosine<0 ? float(3.14159265358979323846)-std::asin(sine) : std::asin(sine);
    auto rotation=GfMatrix4d(GfRotation(GfVec3d(perpendicular),angle*180/3.14159265358979323846),GfVec3d(0));
    auto origin=m.ExtractTranslation();m*=rotation;m.SetTranslateOnly(origin);
}
template<class T> std::vector<T> Collect(const VdfContext &ctx,const TfToken &name) {
    std::vector<T> result;
    for(VdfReadIterator<T> it(ctx,name);!it.IsAtEnd();++it)result.push_back(*it);
    return result;
}
GfMatrix4d ArmatureBlend(const VdfContext &ctx,const GfMatrix4d &incoming) {
    const auto &binds=Collect<GfMatrix4d>(ctx,_constraint->targetBinds);
    const auto &weights=Collect<double>(ctx,_constraint->targetWeights);
    const auto &targetIndices=Collect<int>(ctx,_constraint->targetIndices);
    const auto &objectIndices=Collect<int>(ctx,_constraint->objectIndices);
    std::vector<GfMatrix4d> targets,objects;
    for(VdfReadIterator<rigExec::RigExecPointFrame> it(ctx,_constraint->targets);!it.IsAtEnd();++it)targets.push_back(FrameMatrix(&*it));
    for(VdfReadIterator<rigExec::RigExecPointFrame> it(ctx,_constraint->targetObjects);!it.IsAtEnd();++it)objects.push_back(FrameMatrix(&*it));
    if(binds.size()!=weights.size() || targetIndices.size()!=weights.size() || objectIndices.size()!=weights.size())
        throw std::runtime_error("Armature constraint target array mismatch");
    auto pivot=ctx.GetInputValue<bool>(_constraint->currentPivot)?incoming.ExtractTranslation():
        FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->ownerObject)).Transform(ctx.GetInputValue<GfVec3d>(_constraint->pivot));
    bool dq=ctx.GetInputValue<bool>(_constraint->dualQuaternion);
    GfMatrix4d matrix(0.0),stretchSum(0.0);
    rigExec::RigExecDualQuat sum(GfQuatd(0),GfQuatd(0));
    double total=0;
    for(size_t i=0;i<weights.size();++i) {
        double weight=weights[i];if(weight==0)continue;total+=weight;
        int ti=targetIndices[i],oi=objectIndices[i];
        if(ti<0 || oi<0 || size_t(ti)>=targets.size() || size_t(oi)>=objects.size())throw std::runtime_error("invalid Armature target index");
        const auto delta=objects[oi].GetInverse()*binds[i]*targets[ti];
        if(!dq) {matrix+=delta*weight;continue;}
        // Blender's scale gauge follows the rest bone's Y axis. A generic
        // polar split differs for stretched bones. Retain the residual as
        // an affine matrix, and move its pivot displacement into the rigid
        // part before blending. See docs/references.md.
        auto base=binds[i].GetInverse()*objects[oi];Orthogonalize(base,true);
        auto posed=base*delta;
        // The posed gauge uses Blender's older Y/X orthogonalization,
        // whereas the rest gauge above uses its symmetric stable rule.
        auto y=Row(posed,1);y.Normalize();
        auto z=GfCross(Row(posed,0),y);z.Normalize();
        auto x=GfCross(y,z);x.Normalize();
        SetRow(posed,0,x);SetRow(posed,1,y);SetRow(posed,2,z);
        auto rigid=base.GetInverse()*posed;
        auto stretch=delta*rigid.GetInverse();
        auto shift=stretch.Transform(pivot)-pivot;
        rigid.SetTranslateOnly(rigid.ExtractTranslation()+rigid.TransformDir(shift));
        stretch.SetTranslateOnly(pivot-stretch.TransformDir(pivot));
        auto value=rigExec::RigExecDualQuatFromMatrix(rigid);
        // Source order matters: Blender corrects against the accumulated
        // rotation, rather than choosing a fixed reference target.
        double signedWeight=GfDot(sum.real,value.real)<0?-weight:weight;
        sum.real+=value.real*signedWeight;sum.dual+=value.dual*signedWeight;
        stretchSum+=stretch*weight;
    }
    if(total<=0)return incoming;
    if(dq) {
        auto normalized=sum;
        if(!rigExec::RigExecDualQuatNormalize(&normalized))return incoming;
        matrix=(stretchSum*(1.0/total))*rigExec::RigExecDualQuatToMatrix(normalized);
    } else matrix*=1.0/total;
    return incoming*matrix;
}
GfMatrix4d ConstraintValue(const VdfContext &ctx) {
    auto result=ctx.GetInputValue<GfMatrix4d>(_constraint->incoming);
    auto operation=ctx.GetInputValue<TfToken>(_constraint->operation).GetString();
    if(operation=="PRESERVE_ORIGIN") {
        result.SetTranslateOnly(ctx.GetInputValue<GfMatrix4d>(_constraint->origin).ExtractTranslation());return result;
    }
    if(operation=="LIMIT_ROTATION") {Orthogonalize(result,false);return result;}
    if(operation=="ARMATURE_BLEND")return ArmatureBlend(ctx,result);
    auto source=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->source));
    auto ownerSpace=ctx.GetInputValue<TfToken>(_constraint->ownerSpace).GetString();
    auto targetSpace=ctx.GetInputValue<TfToken>(_constraint->targetSpace).GetString();
    auto ownerObject=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->ownerObject));
    auto sourceObject=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->sourceObject));
    auto customSpace=FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->customSpace));
    bool localTarget=targetSpace=="LOCAL" || targetSpace=="LOCAL_OWNER_ORIENT";
    if(localTarget)source.SetTranslateOnly(source.Transform(ctx.GetInputValue<GfVec3d>(_constraint->targetOffset)));
    ParentSpaces ownerLocal;
    if(ownerSpace=="CUSTOM")result*=customSpace.GetInverse();
    else if(ownerSpace!="WORLD") {
        result*=ownerObject.GetInverse();
        if(ownerSpace=="LOCAL") {
            ownerLocal=ConstraintParentSpaces(ctx,false,ownerObject);
            result=ownerLocal.Apply(result,true);
        }
    }
    if(targetSpace=="CUSTOM")source*=customSpace.GetInverse();
    else if(targetSpace!="WORLD") {
        source*=sourceObject.GetInverse();
        if(targetSpace=="LOCAL" || targetSpace=="LOCAL_OWNER_ORIENT") {
            source=ConstraintParentSpaces(ctx,true,sourceObject).Apply(source,true);
            if(targetSpace=="LOCAL_OWNER_ORIENT") {
                auto difference=ctx.GetInputValue<GfMatrix4d>(_localSpaces->sourceRest)*
                    ctx.GetInputValue<GfMatrix4d>(_localSpaces->ownerRest).GetInverse();
                difference.SetTranslateOnly(GfVec3d(0));
                source=difference.GetInverse()*source*difference;
            }
        }
    }
    auto world=[&](const GfMatrix4d &matrix){
        if(ownerSpace=="WORLD")return matrix;
        if(ownerSpace=="CUSTOM")return matrix*customSpace;
        return (ownerSpace=="LOCAL"?ownerLocal.Apply(matrix):matrix)*ownerObject;
    };
    auto target=localTarget?source.ExtractTranslation():source.Transform(ctx.GetInputValue<GfVec3d>(_constraint->targetOffset));
    if(operation=="COPY_TRANSFORMS") {
        if(ctx.GetInputValue<bool>(_constraint->removeTargetShear))Orthogonalize(source,false);
        auto mix=ctx.GetInputValue<TfToken>(_constraint->rotationMix).GetString();
        if(mix=="REPLACE")return world(source);
        bool before=mix.find("BEFORE")==0;
        if(mix=="BEFORE_FULL" || mix=="AFTER_FULL")return world(before?result*source:source*result);
        auto location=mix.find("SPLIT")!=std::string::npos?result.ExtractTranslation()+source.ExtractTranslation():
            before?source.Transform(result.ExtractTranslation()):result.Transform(source.ExtractTranslation());
        auto size=GfCompMult(Sizes(result),Sizes(source));
        auto a=result,b=source;NormalizeRows(a);NormalizeRows(b);
        if(a.GetDeterminant()<0) {Rescale(a,GfVec3d(-1));size=-size;}
        if(b.GetDeterminant()<0) {Rescale(b,GfVec3d(-1));size=-size;}
        a.SetTranslateOnly(GfVec3d(0));b.SetTranslateOnly(GfVec3d(0));
        auto combined=before?a*b:b*a;Rescale(combined,size);combined.SetTranslateOnly(location);
        return world(combined);
    }
    if(operation=="ARMATURE")
        return result*FrameMatrix(ctx.GetInputValuePtr<rigExec::RigExecPointFrame>(_constraint->sourceObject)).GetInverse()*
            ctx.GetInputValue<GfMatrix4d>(_constraint->inverseBind)*source;
    if(operation=="COPY_LOCATION") {
        auto origin=result.ExtractTranslation();int axes=ctx.GetInputValue<int>(_constraint->axisMask),invert=ctx.GetInputValue<int>(_constraint->invertMask);
        for(int i=0;i<3;++i)if(axes&(1<<i))origin[i]=(invert&(1<<i)?-target[i]:target[i])+(ctx.GetInputValue<bool>(_constraint->offset)?origin[i]:0);
        result.SetTranslateOnly(origin);return world(result);
    }
    if(operation=="COPY_SCALE") {
        auto size=Sizes(source),original=Sizes(result);
        int axes=ctx.GetInputValue<int>(_constraint->axisMask);
        bool uniform=ctx.GetInputValue<bool>(_constraint->uniformScale);
        if(uniform) {
            double product=axes==7?std::abs(source.GetDeterminant()):1;
            if(axes!=7)for(int i=0;i<3;++i)if(axes&(1<<i))product*=size[i];
            size=GfVec3d(std::cbrt(product));
        }
        for(int i=0;i<3;++i) {
            size[i]=std::pow(size[i],ctx.GetInputValue<double>(_constraint->power));
            if(ctx.GetInputValue<bool>(_constraint->offset))
                size[i]=ctx.GetInputValue<bool>(_constraint->scaleAdd)?size[i]+original[i]-1:size[i]*original[i];
            if((uniform || (axes&(1<<i))) && original[i]!=0) SetRow(result,i,Row(result,i)*(size[i]/original[i]));
        }
        return world(result);
    }
    if(operation=="COPY_ROTATION") {
        auto size=Sizes(result),origin=result.ExtractTranslation();
        if(ctx.GetInputValue<int>(_constraint->axisMask)==0)return world(result);
        auto mix=ctx.GetInputValue<TfToken>(_constraint->rotationMix).GetString();
        Orthogonalize(source,true);
        source.SetTranslateOnly(GfVec3d(0));
        if(mix!="REPLACE") {
            auto old=result;NormalizeRows(old);old.SetTranslateOnly(GfVec3d(0));
            if(old.GetDeterminant()<0) {Rescale(old,GfVec3d(-1));size=-size;}
            source=mix=="BEFORE"?old*source:source*old;
        }
        Rescale(source,size);source.SetTranslateOnly(origin);
        return world(source);
    }
    auto direction=target-result.ExtractTranslation();
    if(operation=="DAMPED_TRACK") {
        direction=GfVec3d(GfVec3f(target)-GfVec3f(result.ExtractTranslation()));
        Track(result,direction,ctx.GetInputValue<TfToken>(_constraint->trackAxis).GetString());return result;
    }
    // RigExec point-frame providers require invertible transforms. A zero
    // length Blender stretch collapses the frame; retain the incoming frame
    // in this unsupported case rather than poisoning all downstream skinning.
    double length=ctx.GetInputValue<double>(_constraint->restLength);
    if(direction.GetLength()<1e-6*std::max(1.0,length) || Row(result,1).GetLength()<1e-12) return result;
    auto keep=ctx.GetInputValue<TfToken>(_constraint->keepAxis).GetString();
    if(keep=="SWING_Y") Orthogonalize(result,false);
    auto size=Sizes(result);NormalizeRows(result);
    double distance=direction.Normalize();distance=size[1]!=0?distance/size[1]:0;
    double stretch=distance/length;
    auto volume=ctx.GetInputValue<TfToken>(_constraint->volume).GetString();
    double bulge=volume=="NO_VOLUME"?1:std::pow(length/std::max(distance,1e-30),ctx.GetInputValue<double>(_constraint->bulge));
    double smooth=ctx.GetInputValue<double>(_constraint->bulgeSmooth);
    if(bulge>1 && ctx.GetInputValue<bool>(_constraint->useBulgeMax)) {
        double bound=std::max(1.0,ctx.GetInputValue<double>(_constraint->bulgeMax)),range=bound-1;
        double soft=range>0?1+range*std::atan((bulge-1)/range)*2/3.14159265358979323846:1;
        bulge=(1-smooth)*std::min(bulge,bound)+smooth*soft;
    }
    if(bulge<1 && ctx.GetInputValue<bool>(_constraint->useBulgeMin)) {
        double bound=std::clamp(ctx.GetInputValue<double>(_constraint->bulgeMin),0.0,1.0),range=1-bound;
        double soft=range>0?1-range*std::atan((1-bulge)/range)*2/3.14159265358979323846:1;
        bulge=(1-smooth)*std::max(bulge,bound)+smooth*soft;
    }
    GfVec3d factor(1,stretch,1);
    if(volume=="VOLUME_XZX") factor[0]=factor[2]=std::sqrt(bulge);
    else if(volume=="VOLUME_X")factor[0]=bulge;
    else if(volume=="VOLUME_Z")factor[2]=bulge;
    if(keep=="SWING_Y") Track(result,direction,"TRACK_Y");
    else {
        auto reference=Row(result,keep=="PLANE_X"?0:2);
        auto perpendicular=GfCross(reference,direction);perpendicular.Normalize();
        SetRow(result,1,direction);
        if(keep=="PLANE_X") {SetRow(result,2,perpendicular);auto x=GfCross(direction,perpendicular);x.Normalize();SetRow(result,0,x);}
        else {SetRow(result,0,-perpendicular);auto z=GfCross(direction,perpendicular);z.Normalize();SetRow(result,2,z);}
    }
    Rescale(result,GfVec3d(size[0]*factor[0],size[1]*factor[1],size[2]*factor[2]));
    return result;
}
void PolarFrame(const GfMatrix4d &matrix,GfMatrix3d *rotation,GfMatrix3d *stretch) {
    GfMatrix3d linear(matrix[0][0],matrix[0][1],matrix[0][2],
                      matrix[1][0],matrix[1][1],matrix[1][2],
                      matrix[2][0],matrix[2][1],matrix[2][2]);
    auto orthogonal=linear;
    // Newton's polar iteration for invertible point-frame inputs. Blender
    // obtains the same polar factors using SVD. In USD's row convention the
    // symmetric stretch is on the left: linear = stretch * rotation.
    for(int iteration=0;iteration<64;++iteration) {
        auto next=(orthogonal+orthogonal.GetInverse().GetTranspose())*0.5;
        double difference=0;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)
            difference=std::max(difference,std::abs(next[i][j]-orthogonal[i][j]));
        orthogonal=next;if(difference<1e-12)break;
    }
    *rotation=orthogonal;
    *stretch=linear*orthogonal.GetTranspose();
    // Quaternions require a proper rotation; retain a reflection in stretch.
    if(rotation->GetDeterminant()<0) {*rotation*=-1;*stretch*=-1;}
}
GfMatrix4d BlendFrame(GfMatrix4d before,GfMatrix4d after,double weight) {
    auto location=(1-weight)*before.ExtractTranslation()+weight*after.ExtractTranslation();
    GfMatrix3d rotation0,rotation1,stretch0,stretch1;
    PolarFrame(before,&rotation0,&stretch0);PolarFrame(after,&rotation1,&stretch1);
    auto q0=rotation0.ExtractRotation().GetQuat(),q1=rotation1.ExtractRotation().GetQuat();
    double cosine=GfDot(q0,q1);if(cosine<0){q0=-q0;cosine=-cosine;}
    double w0=1-weight,w1=weight;
    if(cosine<1-1e-4) {
        double angle=std::acos(std::clamp(cosine,-1.0,1.0)),denominator=std::sin(angle);
        w0=std::sin((1-weight)*angle)/denominator;w1=std::sin(weight*angle)/denominator;
    }
    auto linear=((1-weight)*stretch0+weight*stretch1)*GfMatrix3d(w0*q0+w1*q1);
    GfMatrix4d result(1);
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)result[i][j]=linear[i][j];
    result.SetTranslateOnly(location);
    return result;
}
GfMatrix4d ConstraintFrame(const VdfContext &ctx) {
    auto value=ConstraintValue(ctx);
    double influence=ctx.GetInputValue<double>(_constraint->influence);
    if(influence==1)return value;
    auto before=ctx.GetInputValue<GfMatrix4d>(_constraint->incoming);
    // Blender returns the solution to world space before blending influence.
    return BlendFrame(before,value,influence);
}
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecBlenderCopyTransforms) {
    self.AttributeExpression(_armature->matrix).Callback<GfMatrix4d>(&CopyTransforms)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_armature->incoming),Prim().AttributeValue<bool>(_armature->preserveLocation),
            Prim().Relationship(_armature->source).TargetedObjects<rigExec::RigExecPointFrame>(_armature->computePointFrame).InputName(_armature->source));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecBlenderConstraintFrame) {
    self.AttributeExpression(_constraint->matrix).Callback<GfMatrix4d>(&ConstraintFrame)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_constraint->incoming),Prim().AttributeValue<GfMatrix4d>(_constraint->origin),
            Prim().AttributeValue<GfMatrix4d>(_constraint->inverseBind),Prim().AttributeValue<TfToken>(_constraint->operation),
            Prim().AttributeValue<double>(_constraint->influence),
            Prim().AttributeValue<int>(_constraint->targetIndices),Prim().AttributeValue<int>(_constraint->objectIndices),
            Prim().AttributeValue<GfMatrix4d>(_constraint->targetBinds),Prim().AttributeValue<double>(_constraint->targetWeights),
            Prim().AttributeValue<GfVec3d>(_constraint->pivot),Prim().AttributeValue<bool>(_constraint->dualQuaternion),Prim().AttributeValue<bool>(_constraint->currentPivot),
            Prim().Relationship(_constraint->targets).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->targets),
            Prim().Relationship(_constraint->targetObjects).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->targetObjects),
            Prim().AttributeValue<TfToken>(_constraint->ownerSpace),Prim().AttributeValue<TfToken>(_constraint->targetSpace),
            Prim().AttributeValue<int>(_constraint->axisMask),Prim().AttributeValue<int>(_constraint->invertMask),Prim().AttributeValue<bool>(_constraint->offset),
            Prim().AttributeValue<bool>(_constraint->uniformScale),Prim().AttributeValue<bool>(_constraint->scaleAdd),Prim().AttributeValue<double>(_constraint->power),
            Prim().AttributeValue<TfToken>(_constraint->rotationMix),Prim().AttributeValue<bool>(_constraint->removeTargetShear),
            Prim().AttributeValue<TfToken>(_constraint->trackAxis),Prim().AttributeValue<TfToken>(_constraint->keepAxis),Prim().AttributeValue<TfToken>(_constraint->volume),
            Prim().AttributeValue<double>(_constraint->restLength),Prim().AttributeValue<double>(_constraint->bulge),
            Prim().AttributeValue<double>(_constraint->bulgeMin),Prim().AttributeValue<double>(_constraint->bulgeMax),Prim().AttributeValue<double>(_constraint->bulgeSmooth),
            Prim().AttributeValue<bool>(_constraint->useBulgeMin),Prim().AttributeValue<bool>(_constraint->useBulgeMax),Prim().AttributeValue<GfVec3d>(_constraint->targetOffset),
            Prim().Relationship(_constraint->source).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->source),
            Prim().Relationship(_constraint->sourceObject).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->sourceObject),
            Prim().Relationship(_constraint->ownerObject).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->ownerObject),
            Prim().Relationship(_constraint->customSpace).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_constraint->customSpace),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->ownerLocal),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->ownerRest),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->ownerParentRest),
            Prim().AttributeValue<bool>(_localSpaces->ownerHasParent),
            Prim().AttributeValue<bool>(_localSpaces->ownerInheritRotation),
            Prim().AttributeValue<bool>(_localSpaces->ownerLocalLocation),
            Prim().AttributeValue<TfToken>(_localSpaces->ownerInheritScale),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->sourceLocal),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->sourceRest),
            Prim().AttributeValue<GfMatrix4d>(_localSpaces->sourceParentRest),
            Prim().AttributeValue<bool>(_localSpaces->sourceHasParent),
            Prim().AttributeValue<bool>(_localSpaces->sourceInheritRotation),
            Prim().AttributeValue<bool>(_localSpaces->sourceLocalLocation),
            Prim().AttributeValue<TfToken>(_localSpaces->sourceInheritScale),
            Prim().Relationship(_localSpaces->ownerParent).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_localSpaces->ownerParent),
            Prim().Relationship(_localSpaces->sourceParent).TargetedObjects<rigExec::RigExecPointFrame>(_constraint->computePointFrame).InputName(_localSpaces->sourceParent));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecBlenderSkinInfluence) {
    self.AttributeExpression(_skin->matrix).Callback<GfMatrix4d>(&SkinInfluence)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_skin->inverseBind),Prim().AttributeValue<GfMatrix4d>(_skin->inverseMesh),
            Prim().AttributeValue<bool>(_skin->fromBind),Prim().AttributeValue<bool>(_skin->followOnly),
            Prim().Relationship(_skin->owner).TargetedObjects<rigExec::RigExecPointFrame>(_skin->computePointFrame).InputName(_skin->owner),
            Prim().Relationship(_skin->source).TargetedObjects<rigExec::RigExecPointFrame>(_skin->computePointFrame).InputName(_skin->source),
            Prim().Relationship(_skin->sourceObject).TargetedObjects<rigExec::RigExecPointFrame>(_skin->computePointFrame).InputName(_skin->sourceObject));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecBlenderArmatureParent) {
    self.AttributeExpression(_armature->matrix).Callback<GfMatrix4d>(&ArmatureParent)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_armature->local),
            Prim().AttributeValue<GfMatrix4d>(_armature->inverseBind),
            Prim().AttributeValue<GfMatrix4d>(_armature->incoming),Prim().AttributeValue<bool>(_armature->useIncoming),
            Prim().AttributeValue<bool>(_armature->preserveLocation),
            Prim().AttributeValue<double>(_armature->tx),Prim().AttributeValue<double>(_armature->ty),Prim().AttributeValue<double>(_armature->tz),
            Prim().AttributeValue<double>(_armature->rx),Prim().AttributeValue<double>(_armature->ry),Prim().AttributeValue<double>(_armature->rz),
            Prim().AttributeValue<double>(_armature->sx),Prim().AttributeValue<double>(_armature->sy),Prim().AttributeValue<double>(_armature->sz),
            Prim().Relationship(_armature->parent).TargetedObjects<rigExec::RigExecPointFrame>(_armature->computePointFrame).InputName(_armature->parent),
            Prim().Relationship(_armature->sourceObject).TargetedObjects<rigExec::RigExecPointFrame>(_armature->computePointFrame).InputName(_armature->sourceObject),
            Prim().Relationship(_armature->source).TargetedObjects<rigExec::RigExecPointFrame>(_armature->computePointFrame).InputName(_armature->source));
}
EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(RigExecBlenderBoneFrame) {
    self.AttributeExpression(_bone->matrix).Callback<GfMatrix4d>(&BoneFrame)
        .Inputs(Prim().AttributeValue<GfMatrix4d>(_bone->local),Prim().AttributeValue<GfMatrix4d>(_bone->parentRest),
            Prim().AttributeValue<bool>(_bone->hasParent),Prim().AttributeValue<bool>(_bone->inheritRotation),
            Prim().AttributeValue<bool>(_bone->localLocation),Prim().AttributeValue<bool>(_bone->connected),Prim().AttributeValue<TfToken>(_bone->inheritScale),
            Prim().AttributeValue<double>(_bone->tx),Prim().AttributeValue<double>(_bone->ty),Prim().AttributeValue<double>(_bone->tz),
            Prim().AttributeValue<double>(_bone->rx),Prim().AttributeValue<double>(_bone->ry),Prim().AttributeValue<double>(_bone->rz),
            Prim().AttributeValue<double>(_bone->sx),Prim().AttributeValue<double>(_bone->sy),Prim().AttributeValue<double>(_bone->sz),
            Prim().Relationship(_bone->parent).TargetedObjects<rigExec::RigExecPointFrame>(_bone->computePointFrame).InputName(_bone->parent),
            Prim().Relationship(_bone->object).TargetedObjects<rigExec::RigExecPointFrame>(_bone->computePointFrame).InputName(_bone->object));
}
