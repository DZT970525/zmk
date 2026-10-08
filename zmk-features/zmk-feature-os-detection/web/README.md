# zmk-feature-os-detection - Web Frontend

Web UI for `zmk-feature-os-detection`'s Custom Studio RPC subsystem: shows
live USB/BLE OS-detection state and lets you set a per-BLE-profile manual
override. Built from cormoran's ZMK module template.

## Features

- **Device Connection**: Connect to the keyboard over Web Serial
- **State view**: USB connection + detected OS, full BLE profile table
- **BLE overrides**: pin a profile to Windows/macOS/Linux, or back to AUTO
- **react-zmk-studio**: Uses the `@cormoran/zmk-studio-react-hook` library for
  simplified ZMK integration

## Quick Start

```bash
# Install dependencies
npm install

# Generate TypeScript types from proto
npm run generate

# Run development server
npm run dev

# Build for production
npm run build

# Run tests
npm test
```

## Project Structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Connection UI + OsDetectionSection (state/overrides)
├── App.css               # Styles
└── proto/                # Generated protobuf TypeScript types (gitignored)
    └── cormoran/os-detection/
        └── os_detection.ts

test/
├── App.spec.tsx                  # Tests for App component
└── OsDetectionSection.spec.tsx   # Tests for the state table + override RPC
```

## How It Works

### 1. Protocol Definition

The protobuf schema is defined in
`../proto/cormoran/os-detection/os_detection.proto`.

### 2. Code Generation

TypeScript types are generated using `ts-proto`:

```bash
npm run generate
```

This runs `buf generate` which uses the configuration in `buf.gen.yaml`.

### 3. Using react-zmk-studio

The app uses the `@cormoran/zmk-studio-react-hook` library:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

// Connect to device
const { state, connect, findSubsystem, isConnected } = useZMKApp();

// Find your subsystem
const subsystem = findSubsystem("cormoran__os_detection");

// Create service and make RPC calls
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);
const response = await service.callRPC(payload);
```

## Testing

```bash
# Run all tests
npm test

# Run tests in watch mode
npm run test:watch

# Run tests with coverage
npm run test:coverage
```

### Writing Tests

Use the test helpers from `@cormoran/zmk-studio-react-hook/testing`:

```typescript
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";

const mockZMKApp = createConnectedMockZMKApp({
  deviceName: "Test Device",
  subsystems: ["cormoran__os_detection"],
});

render(
  <ZMKAppProvider value={mockZMKApp}>
    <YourComponent />
  </ZMKAppProvider>
);
```

## Publishing

Merging a PR into `main` deploys this app to
https://cormoran.github.io/zmk-feature-os-detection/ via
`.github/workflows/web-ui.yml`. Pull requests get a Cloudflare Workers
preview deployment instead (requires `CLOUDFLARE_API_TOKEN` /
`CLOUDFLARE_ACCOUNT_ID` repo secrets).
