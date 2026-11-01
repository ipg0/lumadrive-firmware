import os
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def provision_drivers(source, target, env):
    del source, target
    configured = os.environ.get("LUMADRIVE_DRIVER_CONFIG") or env.GetProjectOption(
        "custom_driver_config", ""
    )
    if not configured:
        print("Set LUMADRIVE_DRIVER_CONFIG or custom_driver_config to the node header.")
        return 1
    project_dir = Path(env.subst("$PROJECT_DIR"))
    config = Path(configured).expanduser()
    if not config.is_absolute():
        config = project_dir / config
    if not config.is_file():
        print(f"Driver configuration not found: {config}")
        return 1
    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("Specify the target with --upload-port or upload_port.")
        return 1
    return subprocess.run(
        ["bash", str(project_dir / "provisioning/flash_driver_config.sh"), str(config), port],
        check=False,
        cwd=project_dir,
    ).returncode


env.AddCustomTarget(  # type: ignore[name-defined]
    name="provision-drivers",
    dependencies=None,
    actions=[provision_drivers],
    title="Provision drivers",
    description="Flash the selected node's driver wiring partition over USB",
)
