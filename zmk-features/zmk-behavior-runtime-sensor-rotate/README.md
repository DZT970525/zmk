# ZMK Runtime Sensor Rotate Module

This ZMK module provides a runtime-configurable sensor rotation behavior with web UI support.
It allows you to configure sensor rotation bindings per layer via ZMK Studio's custom RPC protocol,
with persistent storage.

## Features

- **Runtime Configuration**: Configure sensor rotation bindings without reflashing firmware
- **Default Bindings**: Define static fallback bindings in device tree for immediate functionality
- **Per-Layer Bindings**: Different bindings for each keyboard layer
- **Persistent Storage**: Configuration is saved and restored across reboots
- **Web UI**: Easy-to-use web interface for configuration

## Setup

### 1. Add dependency to your `config/west.yml`

```yaml
manifest:
  remotes:
    ...
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    ...
    - name: zmk-behavior-runtime-sensor-rotate
      remote: cormoran
      revision: main # or latest commit hash
    # Required: Use ZMK with custom studio protocol support
    - name: zmk
      remote: cormoran
      revision: v0.3+custom-studio-protocol
      import:
        file: app/west.yml
```

### 2. Enable in your `config/<shield>.conf`

```conf
# Enable ZMK Studio
CONFIG_ZMK_STUDIO=y

# Enable the runtime sensor rotate module
CONFIG_ZMK_RUNTIME_SENSOR_ROTATE=y
CONFIG_ZMK_RUNTIME_SENSOR_ROTATE_STUDIO_RPC=y

# Enable settings storage (if not already enabled)
CONFIG_SETTINGS=y
```

### 3. Add to your keymap

In your `<keyboard>.keymap`, define a runtime sensor rotate behavior instance:

```dts
#include <behaviors/runtime-sensor-rotate.dtsi>


/ {
    behaviors {
        // Optionally define default bindings
        rsr_vol: rsr_vol {
            compatible = "zmk,behavior-runtime-sensor-rotate";
            #sensor-binding-cells = <0>;
            tap-ms = <5>;
            cw-binding = <&kp C_VOL_UP>;
            ccw-binding = <&kp C_VOL_DN>;
        };
    };
};

/ {
	keymap {
		compatible = "zmk,keymap";
        default_layer {
            ...
            // If you have two sensors
            sensor-bindings = <&rsr_trans &rsr_trans>;
        }
        ...
        layer_n {
            ...
            // Set default binding for layer_n's sensor0
            sensor-bindings = <&rsr_vol &rsr_trans>;
        }
    }
}
```

**Behavior Properties:**

- `tap-ms`: Duration in milliseconds for each trigger press (default: 5)
- `cw-binding` (optional): Default binding for clockwise rotation. Used as fallback when no runtime binding is configured for a layer.
- `ccw-binding` (optional): Default binding for counter-clockwise rotation. Used as fallback when no runtime binding is configured for a layer.

**Note:** Default bindings are optional. If not specified, the behavior will only respond to runtime-configured bindings set via the Web UI. If neither default nor runtime bindings are configured, the behavior is transparent (no action is taken).

### Web UI Configuration

1. **Build and deploy the Web UI**:

   ```bash
   cd web
   npm install
   npm run build
   ```

2. **Connect to your keyboard**:
   - Open the web UI in your browser
   - Click "Connect Serial" to connect via USB serial
   - The web UI will detect the custom subsystem

3. **Configure bindings**:
   - Select the sensor index (0 or 1)
   - Click "Load Configuration" to fetch current bindings
   - Select the layer you want to configure
   - Set clockwise and counter-clockwise bindings:
     - Select behavior from the dropdown (e.g., "kp" for key press)
     - Set param1 and param2 as needed
   - Click "Save Bindings" to persist the configuration

**Note:** Runtime bindings configured via Web UI override default bindings specified in device tree.

## Development

### Repository Structure

```
.
├── dts/                    # Device tree bindings
├── include/                # Public header files
├── src/
│   ├── behaviors/         # Behavior implementation
│   └── studio/            # RPC handlers
├── proto/                 # Protocol buffer definitions
└── web/                   # Web UI
    ├── src/
    │   ├── RuntimeSensorRotateConfig.tsx  # Main UI component
    │   └── proto/         # Generated TypeScript types
    └── test/              # Web UI tests
```

### Web UI Development

```bash
cd web
npm install
npm run dev      # Start development server
npm run build    # Build for production
npm test         # Run tests
```

## Development Guide

### Pre-commit Hooks

This repository uses [pre-commit](https://pre-commit.com/) hooks to ensure code quality before commits. The hooks automatically check and fix:

- Trailing whitespace
- End of file fixes
- YAML syntax
- Large files
- Merge conflicts
- **Web code**: Prettier formatting, ESLint linting, Jest tests
- **ZMK module**: Python unit tests

**Setup pre-commit hooks:**

```bash
# Install pre-commit (if not already installed)
pip install pre-commit

# Install the git hooks
pre-commit install
```

**Running pre-commit manually:**

```bash
# Run on all files
pre-commit run --all-files

# Run on staged files only (happens automatically on commit)
pre-commit run
```

The pre-commit hooks are automatically configured in the Copilot GitHub Actions environment and will run on all commits in CI.

### Setup

There are two west workspace layout options.

#### Option1: Download dependencies in parent directory

This option is west's standard way. Choose this option if you want to re-use dependent projects in other zephyr module development.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf tests/west-test.yml
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
west-workspace
  - .west/config
  - build : build output directory
  - <this repository>
  # other dependencies
  - zmk
  - zephyr
  - ...
  # You can develop other zephyr modules in this workspace
  - your-other-repo
```

You can switch between modules by removing `west-workspace/.west` and re-executing `west init ...`.

#### Option2: Download dependencies in ./dependencies (Enabled in dev-container)

Choose this option if you want to download dependencies under this directory (like node_modules in npm). This option is useful for specifying cache target in CI. The layout is relatively easy to recognize if you want to isolate dependencies.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-standalone.yml
# If you use dev container, start from below commands. Above commands are executed
# automatically.
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
<this repository>
  - .west/config
  - build : build output directory
  - dependencies
    - zmk
    - zephyr
    - ...
```

### Dev container

Dev container is configured for setup option2. The container creates below volumes to re-use resources among containers.

- zmk-dependencies: dependencies dir for setup option2
- zmk-build: build output directory
- zmk-root-user: /root, the same to ZMK's official dev container

### Web UI

Please refer [./web/README.md](./web/README.md).

## Test

**ZMK firmware test**

`./tests` directory contains test config for posix to confirm module functionality and config for xiao board to confirm build works.

Tests can be executed by below command:

```bash
# Run all test case and verify results
python -m unittest
```

If you want to execute west command manually, run below. (for zmk-build, the result is not verified.)

```
# Build test firmware for xiao
# `-m tests/zmk-config .` means tests/zmk-config and this repo are added as additional zephyr module
west zmk-build tests/zmk-config/config -m tests/zmk-config .

# Run zmk test cases
# -m . is required to add this module to build
west zmk-test tests -m .
```

**Web UI test**

The `./web` directory includes Jest tests. See [./web/README.md](./web/README.md#testing) for more details.

```bash
cd web
npm test
```

## Publishing Web UI

### GitHub Pages (Production)

Github actions are pre-configured to publish web UI to github pages.

1. Visit Settings>Pages
1. Set source as "Github Actions"
1. Visit Actions>"Test and Build Web UI"
1. Click "Run workflow"

Then, the Web UI will be available in
`https://<your github account>.github.io/<repository name>/` like https://cormoran.github.io/zmk-module-template-with-custom-studio-rpc.

### Cloudflare Workers (Pull Request Preview)

For previewing web UI changes in pull requests:

1. Create a Cloudflare Workers project and configure secrets:
   - `CLOUDFLARE_API_TOKEN`: API token with Cloudflare Pages edit permission
   - `CLOUDFLARE_ACCOUNT_ID`: Your Cloudflare account ID
   - (Optional) `CLOUDFLARE_PROJECT_NAME`: Project name (defaults to `zmk-module-web-ui`)
   - Enable "Preview URLs" feature in cloudflare the project

2. Optionally set up an `approval-required` environment in github repository settings requiring approval from repository owners

3. Create a pull request with web UI changes - the preview deployment will trigger automatically and wait for approval

## Sync changes in template

By running `Actions > Sync Changes in Template > Run workflow`, pull request is created to your repository to reflect changes in template repository.

If the template contains changes in `.github/workflows/*`, registering your github personal access token as `GH_TOKEN` to repository secret is required.
The fine-grained token requires write to contents, pull-requests and workflows.
Please see detail in [actions-template-sync](https://github.com/AndreasAugustin/actions-template-sync).

## More Info

For more info on modules, you can read through through the
[Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html)
and [ZMK's page on using modules](https://zmk.dev/docs/features/modules).
[Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests)
may also be of use.
