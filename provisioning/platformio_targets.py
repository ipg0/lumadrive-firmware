import os
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def provision_hardware(source, target, env):
    del source, target
    configured = os.environ.get("LUMADRIVE_HARDWARE_CONFIG") or env.GetProjectOption(
        "custom_hardware_config", ""
    )
    if not configured:
        print("Set LUMADRIVE_HARDWARE_CONFIG or custom_hardware_config to the node header.")
        return 1
    project_dir = Path(env.subst("$PROJECT_DIR"))
    config = Path(configured).expanduser()
    if not config.is_absolute():
        config = project_dir / config
    if not config.is_file():
        print(f"Hardware configuration not found: {config}")
        return 1
    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("Specify the target with --upload-port or upload_port.")
        return 1
    return subprocess.run(
        ["bash", str(project_dir / "provisioning/flash_hardware_config.sh"), str(config), port],
        check=False,
        cwd=project_dir,
    ).returncode


env.AddCustomTarget(  # type: ignore[name-defined]
    name="provision-hardware",
    dependencies=None,
    actions=[provision_hardware],
    title="Provision hardware",
    description="Flash the selected node's hardware configuration over USB",
)
