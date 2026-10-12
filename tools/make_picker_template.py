"""Regenerate resources/biped_picker.usda from the usdRig biped example pickers.

    python tools/make_picker_template.py ../usdRig/examples/biped resources/biped_picker.usda
    python tools/make_picker_template.py ../usdRig/examples/biped resources/biped_picker.usda --check

The template is the native Body and Face panels with every control selection
replaced by the role it stands for. Run it in the host USD Python environment;
it reads the two example layers as plain Sdf data and needs no plugin.
"""
import sys

from pxr import Gf, Sdf, Vt

DOC = ("Native biped Body/Facial artwork adapted from usdRig examples/biped. customData blender.roles names the native "
       "biped control(s) a slot selects; the translator resolves each role to a source control through its "
       "rig-generator name table. The template carries role keys only, no source bone names. Unresolved slots are "
       "not authored.")
PANELS = (('Biped_body_picker.usda', '/Biped/Rig/Uman/Body', 'Body'),
          ('Biped_face_picker.usda', '/Biped/Rig/Uman/Facial', 'Facial'))
# Fallbacks of the RigExecPickerButton schema; the translator authors no value equal to one.
FALLBACK = {'ui:fill': Gf.Vec4f(0.5, 0.5, 0.5, 1), 'ui:fontSize': 8.0, 'ui:position': Gf.Vec2f(0, 0),
            'ui:rotation': 0.0, 'ui:roundness': 0.0, 'ui:shape': 'rectangle', 'ui:size': Gf.Vec2f(10, 10),
            'ui:slopeLeft': 0.0, 'ui:slopeRight': 0.0, 'ui:stroke': Gf.Vec4f(0, 0, 0, 0.8), 'ui:strokeWidth': 1.0,
            'ui:text': '', 'ui:textAlign': 'center', 'ui:textColor': Gf.Vec4f(0, 0, 0, 1),
            'ui:valueColor': Gf.Vec4f(0, 0, 0, 1), 'ui:depth': 0.0, 'ui:direction': 'top', 'ui:bold': False,
            'ui:alternate': False}
PANEL_SIZE = Gf.Vec2f(400, 600)
MIRROR = 'rigExec:picker:mirror'
# The native layout stacks each IK slot on its FK slot behind a mode dial. A
# translated rig has no dial and the stock picker gives a click to the topmost
# box, so each pair shares the FK rectangle side by side and the knee pole
# draws above the FK thigh. The three native pairs without a mirror link get one.
HALVES = (('Hand', 'ArmIK'), ('Foot', 'LegIK'))
RAISED = (('LegPV', 'UpLeg'),)
MIRRORED = ('Hand', 'ArmIK', 'UpArmSwing')


def value(button, name):
    attribute = button.attributes.get(name)
    return FALLBACK.get(name) if attribute is None else attribute.default


def build(examples):
    layer = Sdf.Layer.CreateAnonymous('.usda')
    layer.documentation = DOC
    template = Sdf.CreatePrimInLayer(layer, '/Template')
    template.specifier, template.typeName = Sdf.SpecifierDef, 'RigExecPicker'
    for order, (name, root, panel_name) in enumerate(PANELS):
        native = Sdf.Layer.FindOrOpen(examples.rstrip('/') + '/' + name)
        if not native or not native.GetPrimAtPath(root):
            raise SystemExit('cannot read %s from %s' % (root, examples.rstrip('/') + '/' + name))
        path = Sdf.Path('/Template').AppendChild(panel_name)
        Sdf.CopySpec(native, root, layer, path)
        panel = layer.GetPrimAtPath(path)
        for attribute in list(panel.attributes):
            if attribute.name == 'ui:order' or (attribute.name, attribute.default) == ('ui:size', PANEL_SIZE):
                panel.RemoveProperty(attribute)
        if order:
            Sdf.AttributeSpec(panel, 'ui:order', Sdf.ValueTypeNames.Int, Sdf.VariabilityUniform).default = order
        dropped = []
        for button in panel.nameChildren:
            controls = button.relationships.get('rigExec:picker:controls')
            roles = [target.name for target in controls.targetPathList.explicitItems] if controls else []
            if roles:
                button.customData = {'blender': {'roles': Vt.StringArray(roles)}}
            elif (value(button, 'ui:text') or value(button, 'ui:shape') == 'slider' or value(button, 'ui:alternate')
                  or 'rigExec:picker:command' in button.attributes
                  or any(r.name != MIRROR for r in button.relationships)):
                # Labels, commands, attribute slots, sliders and mode-only
                # artwork: nothing a translated rig can drive or the stock picker draws.
                dropped.append(button.name)
                continue
            for relationship in list(button.relationships):
                if relationship.name != MIRROR:
                    button.RemoveProperty(relationship)
            for attribute in list(button.attributes):
                if attribute.name.startswith('rigExec:') or (
                        attribute.name in FALLBACK and attribute.default == FALLBACK[attribute.name]):
                    button.RemoveProperty(attribute)
        for name in dropped:
            del panel.nameChildren[name]
        for button in panel.nameChildren:
            mirror = button.relationships.get(MIRROR)
            if mirror:
                peers = [path.AppendChild(target.name) for target in mirror.targetPathList.explicitItems
                         if target.name in panel.nameChildren]
                mirror.custom = True
                if peers:
                    mirror.targetPathList.explicitItems = peers
                else:
                    button.RemoveProperty(mirror)
    body = layer.GetPrimAtPath('/Template/Body')

    def slot(side, role):
        return body.nameChildren['b_%s_%s' % (side, role)]

    def set_value(button, name, type_name, new):
        attribute = button.attributes.get(name) or Sdf.AttributeSpec(button, name, type_name, Sdf.VariabilityUniform)
        attribute.default = new

    for side, other in (('L', 'R'), ('R', 'L')):
        for fk, ik in HALVES:
            position, size = value(slot(side, fk), 'ui:position'), value(slot(side, fk), 'ui:size')
            half = Gf.Vec2f(size[0] / 2, size[1])
            set_value(slot(side, fk), 'ui:size', Sdf.ValueTypeNames.Float2, half)
            set_value(slot(side, ik), 'ui:position', Sdf.ValueTypeNames.Float2, Gf.Vec2f(position[0] + half[0], position[1]))
            set_value(slot(side, ik), 'ui:size', Sdf.ValueTypeNames.Float2, half)
            # Side by side they are no longer alternate states of one slot.
            if 'ui:alternate' in slot(side, ik).attributes:
                slot(side, ik).RemoveProperty(slot(side, ik).attributes['ui:alternate'])
        for above, below in RAISED:
            set_value(slot(side, above), 'ui:depth', Sdf.ValueTypeNames.Float,
                      round(value(slot(side, below), 'ui:depth') + 0.0001, 4))
        for role in MIRRORED:
            mirror = slot(side, role).relationships.get(MIRROR) or Sdf.RelationshipSpec(slot(side, role), MIRROR, custom=True)
            mirror.targetPathList.explicitItems = [slot(other, role).path]
    return layer


def main():
    arguments = [a for a in sys.argv[1:] if a != '--check']
    if len(arguments) != 2:
        raise SystemExit(__doc__)
    layer = build(arguments[0])
    if '--check' in sys.argv[1:]:
        current = Sdf.Layer.FindOrOpen(arguments[1])
        if not current or current.ExportToString() != layer.ExportToString():
            raise SystemExit(arguments[1] + ' differs from the regenerated template')
        print(arguments[1] + ' matches the regenerated template')
    elif not layer.Export(arguments[1]):
        raise SystemExit('cannot write ' + arguments[1])


if __name__ == '__main__':
    main()
