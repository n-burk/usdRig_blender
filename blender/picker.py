"""Native picker inventory; reads source data without executing rig UI scripts."""
import json

CONTROL_COLLECTIONS = {
    'Face Main', 'Face Secondary', 'Face Tweak', 'Face Silhouette', 'Hair',
    'Clothes Main', 'Fannypack Buckle', 'Lips', 'Tracking', 'Curve Hooks',
    'Curve Handles',
}
SYSTEM_PROPERTIES = {'bone_gizmo', 'enable_bone_gizmo', 'prop_hierarchy'}


def control_collection(name):
    return 'Control' in name or name in CONTROL_COLLECTIONS


def classify(records):
    """Keep shape and collection evidence independent of emitted USD controls."""
    controls, excluded = [], []
    for record in records:
        member = any(control_collection(c) for c in record['collections'])
        editable = any(not v for values in record['locks'].values() for v in values)
        properties = record.get('properties', {})
        if (record['shape'] or member) and (editable or properties):
            controls.append(record)
        else:
            excluded.append({'name': record['name'], 'reason':
                'no editable channels' if record['shape'] or member else
                'no custom shape or explicit control collection'})
    return controls, excluded


def ellie_picker(obj, object_id, bone_id):
    # Identify the generated main rig; never include its META construction rig.
    if obj.name != 'RIG-Ellie' or not {'MSTR-Spine_Torso', 'FK-Head', 'Properties_Character_Ellie'}.issubset(obj.pose.bones.keys()):
        return None
    records = []
    for p in obj.pose.bones:
        properties = {}
        for key in p.keys():
            if key in SYSTEM_PROPERTIES or key.startswith('$'):
                continue
            value = p[key]
            if isinstance(value, (bool, int, float)):
                try:
                    meta = p.id_properties_ui(key).as_dict()
                except (TypeError, KeyError):
                    meta = {}
                properties[key] = {'value': value, 'description': meta.get('description', ''),
                    'min': meta.get('min'), 'max': meta.get('max')}
        records.append({'name': p.name, 'id': bone_id(obj, p.name),
            'shape': p.custom_shape.name if p.custom_shape else '',
            'collections': [c.name for c in p.bone.collections],
            'locks': {'translation': list(p.lock_location), 'rotation': list(p.lock_rotation),
                      'scale': list(p.lock_scale)},
            'hidden': bool(p.bone.hide or not any(c.is_visible_effectively for c in p.bone.collections)),
            'properties': properties})
    controls, excluded = classify(records)
    groups = {}
    for record in controls:
        # Preserve all memberships in inventory; one library entry per control.
        named = [c for c in record['collections'] if not c.startswith('Layer ')]
        preferred = [c for c in named if control_collection(c)]
        group = (preferred or named or ['Other controls'])[0]
        groups.setdefault(group, []).append({'label': record['name'], 'controls': [record['id']]})
    pages = []
    for name, buttons in sorted(groups.items()):
        ordered = sorted(buttons, key=lambda b: b['label'])
        chunks = [ordered[i:i+48] for i in range(0, len(ordered), 48)]
        for index, chunk in enumerate(chunks):
            label = name if len(chunks) == 1 else name + ' ' + str(index+1) + '/' + str(len(chunks))
            pages.append({'name': label, 'buttons': chunk})
    settings = []
    eligible = {r['name'] for r in controls} | {'Properties_Character_Ellie'}
    for record in records:
        if record['name'] not in eligible:
            continue
        for key, value in sorted(record['properties'].items()):
            settings.append({'label': record['name'] + ': ' + key, 'controls': [],
                'source': {'binding': {'owner': record['id'], 'property': key, **value},
                    'unsupported_reason': 'Source property driver behavior is not translated to a native editable attribute'}})
    for i in range(0, len(settings), 48):
        pages.append({'name': 'Source settings (unavailable) ' + str(i//48+1),
                      'buttons': settings[i:i+48]})
    return {'owner': object_id(obj), 'name': 'Ellie', 'source_kind': 'Ellie biped and all source controls',
        'layout': 'biped', 'source_ui': '', 'inventory': {'armature': obj.name,
            'controls': controls, 'excluded': excluded, 'source_bones': len(records),
            'shape_controls': sum(bool(r['shape']) for r in controls),
            'transformControls': len(controls), 'functionalSettings': 0,
            'unavailableSettings': len(settings)}, 'pages': pages}
