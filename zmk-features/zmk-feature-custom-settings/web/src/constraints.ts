import type {
  SettingConstraint,
  SettingConstraintOptions,
  SettingMeta,
  SettingScalarValue,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";

// Mirrors the firmware/proto cap on repeated option values/labels
// (proto `options` uses max_count:8). Kept in sync manually because the
// generated TS bindings do not surface nanopb max_count.
export const OPTIONS_MAX_COUNT = 8;

export type ConstraintKind =
  | "options"
  | "range"
  | "hidUsage"
  | "layerId"
  | "behaviorId";

/**
 * Returns the first recognized constraint carried by a setting's metadata, or
 * undefined when no constraint applies. The firmware may advertise several
 * constraints; we drive the editor from the first one we know how to render.
 */
export function getActiveConstraint(
  meta: SettingMeta | undefined
): SettingConstraint | undefined {
  if (!meta) return undefined;
  return meta.constraints.find((constraint) => constraintKind(constraint));
}

export function constraintKind(
  constraint: SettingConstraint | undefined
): ConstraintKind | undefined {
  if (!constraint) return undefined;
  if (constraint.options) return "options";
  if (constraint.range) return "range";
  if (constraint.hidUsage) return "hidUsage";
  if (constraint.layerId) return "layerId";
  if (constraint.behaviorId) return "behaviorId";
  return undefined;
}

/** Renders a scalar constraint bound into the same string the editor uses. */
export function scalarValueToEditorString(value: SettingScalarValue): string {
  if (value.int32Value !== undefined) return `${value.int32Value}`;
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.bytesValue !== undefined) {
    return Array.from(value.bytesValue)
      .map((byte) => byte.toString(16).padStart(2, "0"))
      .join(" ");
  }
  if (value.behaviorValue !== undefined) {
    const { behaviorId, param1, param2 } = value.behaviorValue;
    return `${behaviorId},${param1},${param2}`;
  }
  return "";
}

export function scalarValueToNumber(
  value: SettingScalarValue | undefined
): number | undefined {
  if (!value) return undefined;
  if (value.int32Value !== undefined) return value.int32Value;
  return undefined;
}

export type OptionEntry = { value: string; label: string };

/** Pairs each option value with its human label (falling back to the value). */
export function optionEntries(
  options: SettingConstraintOptions
): OptionEntry[] {
  return options.values.map((value, index) => {
    const editorValue = scalarValueToEditorString(value);
    const label = options.labels[index];
    return {
      value: editorValue,
      label: label && label.length > 0 ? label : editorValue,
    };
  });
}

/**
 * Heuristic: the RPC truncates the option list to OPTIONS_MAX_COUNT, so a full
 * list of exactly that length may have been clipped. We cannot know for sure
 * from the wire, so this is surfaced only as a hint.
 */
export function isOptionsPossiblyTruncated(
  options: SettingConstraintOptions
): boolean {
  return options.values.length >= OPTIONS_MAX_COUNT;
}

/**
 * Client-side validation of the editor's raw string against the advertised
 * constraint. Returns a human-readable message when the value violates the
 * constraint, or null when it is acceptable (or unconstrained).
 */
export function validateConstraintValue(
  constraint: SettingConstraint | undefined,
  raw: string
): string | null {
  if (!constraint) return null;

  if (constraint.options) {
    const entries = optionEntries(constraint.options);
    if (entries.some((entry) => entry.value === raw)) {
      return null;
    }
    const choices = entries.map((entry) => entry.label).join(", ");
    return choices ? `Select one of: ${choices}.` : "Select a value.";
  }

  if (constraint.range) {
    const parsed = parseNumber(raw);
    if (parsed === undefined) return "Enter a number.";
    const min = scalarValueToNumber(constraint.range.min);
    const max = scalarValueToNumber(constraint.range.max);
    if (min !== undefined && parsed < min) return `Value must be ≥ ${min}.`;
    if (max !== undefined && parsed > max) return `Value must be ≤ ${max}.`;
    return null;
  }

  if (constraint.hidUsage) {
    const parsed = parseNumber(raw);
    if (parsed === undefined) return "Enter a usage number.";
    const { usageMin, usageMax } = constraint.hidUsage;
    if (parsed < usageMin) return `Usage must be ≥ ${usageMin}.`;
    if (usageMax > 0 && parsed > usageMax) {
      return `Usage must be ≤ ${usageMax}.`;
    }
    return null;
  }

  if (constraint.layerId) {
    const parsed = parseNumber(raw);
    if (parsed === undefined || !Number.isInteger(parsed) || parsed < 0) {
      return "Enter a non-negative layer index.";
    }
    return null;
  }

  if (constraint.behaviorId) {
    if (!/^\s*\d+\s*(,\s*\d+\s*){0,2}$/.test(raw)) {
      return "Enter behaviorId,param1,param2 as numbers.";
    }
    return null;
  }

  return null;
}

function parseNumber(raw: string): number | undefined {
  const trimmed = raw.trim();
  if (trimmed.length === 0) return undefined;
  const parsed = Number(trimmed);
  return Number.isNaN(parsed) ? undefined : parsed;
}
