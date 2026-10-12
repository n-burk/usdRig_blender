// usdRig installs its USD scene lowering when librigExecScene loads. Its own
// targets compile libs/rigExecScene/sceneLinkAnchor.cpp into each consumer; an
// installed package carries neither that file nor a declaration, so the
// converter and its evaluating tests mirror it here. The symbol is usdRig's:
// if it is renamed there, this file fails to link and must follow.
namespace rigExec {
void RigExecSceneLinkAnchor();
namespace {
const struct SceneLink { SceneLink() { RigExecSceneLinkAnchor(); } } sceneLink;
}
}
