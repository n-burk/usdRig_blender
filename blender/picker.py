"""Native picker inventory; reads source data without executing rig UI scripts."""
import json

SYSTEM_PROPERTIES = {'bone_gizmo', 'enable_bone_gizmo', 'prop_hierarchy'}


def control_collections(records):
    """Collections that hold controls: most of their bones carry a custom shape.

    Measured on the rig itself, so no collection is recognised by its name.
    """
    bones, shaped = {}, {}
    for record in records:
        for name in record['collections']:
            bones[name] = bones.get(name, 0) + 1
            shaped[name] = shaped.get(name, 0) + bool(record['shape'])
    return {name for name, count in bones.items() if 2 * shaped[name] > count}


def classify(records):
    """Keep shape and collection evidence independent of emitted USD controls.

    A control has a custom shape, or sits unshaped in a control collection;
    either way it needs an editable channel or a numeric property.
    """
    controls, excluded = [], []
    member_of = control_collections(records)
    for record in records:
        member = any(c in member_of for c in record['collections'])
        editable = any(not v for values in record['locks'].values() for v in values)
        properties = record.get('properties', {})
        if (record['shape'] or member) and (editable or properties):
            controls.append(record)
        else:
            excluded.append({'name': record['name'], 'reason':
                'no editable channels' if record['shape'] or member else
                'no custom shape or control collection'})
    return controls, excluded


def collection_picker(obj, object_id, bone_id):
    """Source pages for a generated rig that stores no sidebar pages of its own.

    One page per control collection, one entry per classified control, plus
    one page of the numeric source settings as unavailable entries. Pages are
    whole collections: the translator maps them onto the biped panels and
    cuts what is left into panels, so nothing here depends on the rig name or
    on a page size.
    """
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
    member_of = control_collections(records)
    groups = {}
    for record in controls:
        # Preserve all memberships in inventory; one library entry per control.
        named = [c for c in record['collections'] if not c.startswith('Layer ')]
        preferred = [c for c in named if c in member_of]
        group = (preferred or named or ['Other controls'])[0]
        groups.setdefault(group, []).append({'label': record['name'], 'controls': [record['id']]})
    pages = [{'name': name, 'source': {'collection': name}, 'buttons': sorted(buttons, key=lambda b: b['label'])}
             for name, buttons in sorted(groups.items())]
    settings = []
    eligible = {r['name'] for r in controls}
    for record in records:
        # Generated rigs keep character settings on an unshaped Properties bone.
        if record['name'] not in eligible and not record['name'].startswith('Properties'):
            continue
        for key, value in sorted(record['properties'].items()):
            settings.append({'label': record['name'] + ': ' + key, 'controls': [],
                'source': {'binding': {'owner': record['id'], 'property': key, **value},
                    'unsupported_reason': 'Source property driver behavior is not translated to a native editable attribute'}})
    if settings:
        pages.append({'name': 'Source settings (unavailable)', 'buttons': settings})
    if not pages:
        return None
    return {'owner': object_id(obj), 'name': obj.name, 'source_kind': 'Control collections',
        'source_ui': '', 'inventory': {'armature': obj.name,
            'controls': controls, 'excluded': excluded, 'source_bones': len(records),
            'shape_controls': sum(bool(r['shape']) for r in controls),
            'transformControls': len(controls), 'functionalSettings': 0,
            'unavailableSettings': len(settings)}, 'pages': pages}
