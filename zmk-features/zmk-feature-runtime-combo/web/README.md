# Runtime Combo Web Frontend

React + TypeScript frontend for editing `zmk-feature-runtime-combo` through the
custom ZMK Studio RPC subsystem.

## Quick Start

```bash
npm install
npm run generate
npm run dev
```

## Commands

```bash
npm run generate  # Generate TypeScript protobuf types from ../proto
npm run build     # Type-check and build
npm test          # Run Jest tests
npm run lint      # ESLint and Prettier check
```

## Protocol

The protobuf schema is defined at:

```text
../proto/cormoran/runtime_combo/runtime_combo.proto
```

The generated TypeScript file is:

```text
src/proto/cormoran/runtime_combo/runtime_combo.ts
```

The custom subsystem identifier is:

```text
cormoran__runtime_combo
```
