import {
  getActiveConstraint,
  constraintKind,
  optionEntries,
  isOptionsPossiblyTruncated,
  scalarValueToEditorString,
  scalarValueToNumber,
  validateConstraintValue,
} from "../src/constraints";
import type {
  SettingConstraint,
  SettingMeta,
} from "../src/proto/cormoran/zmk/custom_settings/custom_settings";

function meta(constraints: SettingConstraint[]): SettingMeta {
  return {
    confidentiality: 0,
    readPermission: 0,
    writePermission: 0,
    constraints,
  };
}

describe("getActiveConstraint", () => {
  it("returns undefined without meta", () => {
    expect(getActiveConstraint(undefined)).toBeUndefined();
  });

  it("returns the first recognized constraint", () => {
    const range: SettingConstraint = {
      range: { min: { int32Value: 0 }, max: { int32Value: 3 } },
    };
    expect(getActiveConstraint(meta([{}, range]))).toBe(range);
  });
});

describe("constraintKind", () => {
  it("maps each constraint oneof to its kind", () => {
    expect(constraintKind({ options: { values: [], labels: [] } })).toBe(
      "options"
    );
    expect(constraintKind({ range: { min: undefined, max: undefined } })).toBe(
      "range"
    );
    expect(
      constraintKind({ hidUsage: { usagePage: 7, usageMin: 0, usageMax: 5 } })
    ).toBe("hidUsage");
    expect(constraintKind({ layerId: {} })).toBe("layerId");
    expect(constraintKind({ behaviorId: {} })).toBe("behaviorId");
    expect(constraintKind({})).toBeUndefined();
  });
});

describe("optionEntries", () => {
  it("pairs values with labels, falling back to the value string", () => {
    expect(
      optionEntries({
        values: [{ int32Value: 1 }, { int32Value: 2 }],
        labels: ["Low", ""],
      })
    ).toEqual([
      { value: "1", label: "Low" },
      { value: "2", label: "2" },
    ]);
  });
});

describe("isOptionsPossiblyTruncated", () => {
  it("is true only at the RPC cap", () => {
    expect(
      isOptionsPossiblyTruncated({
        values: Array.from({ length: 8 }, (_, i) => ({ int32Value: i })),
        labels: [],
      })
    ).toBe(true);
    expect(
      isOptionsPossiblyTruncated({
        values: [{ int32Value: 0 }],
        labels: [],
      })
    ).toBe(false);
  });
});

describe("scalar helpers", () => {
  it("stringifies scalar values", () => {
    expect(scalarValueToEditorString({ int32Value: 5 })).toBe("5");
    expect(scalarValueToEditorString({ boolValue: true })).toBe("true");
    expect(scalarValueToEditorString({ stringValue: "hi" })).toBe("hi");
  });

  it("extracts numeric bounds", () => {
    expect(scalarValueToNumber({ int32Value: 9 })).toBe(9);
    expect(scalarValueToNumber({ stringValue: "x" })).toBeUndefined();
    expect(scalarValueToNumber(undefined)).toBeUndefined();
  });
});

describe("validateConstraintValue", () => {
  it("passes through when unconstrained", () => {
    expect(validateConstraintValue(undefined, "anything")).toBeNull();
  });

  it("requires an allowed option", () => {
    const options: SettingConstraint = {
      options: {
        values: [{ int32Value: 1 }, { int32Value: 2 }],
        labels: ["Low", "High"],
      },
    };
    expect(validateConstraintValue(options, "2")).toBeNull();
    expect(validateConstraintValue(options, "3")).toMatch(/Low, High/);
  });

  it("enforces range bounds", () => {
    const range: SettingConstraint = {
      range: { min: { int32Value: 0 }, max: { int32Value: 10 } },
    };
    expect(validateConstraintValue(range, "5")).toBeNull();
    expect(validateConstraintValue(range, "-1")).toMatch(/≥ 0/);
    expect(validateConstraintValue(range, "11")).toMatch(/≤ 10/);
    expect(validateConstraintValue(range, "")).toMatch(/number/);
  });

  it("enforces hid usage bounds", () => {
    const hid: SettingConstraint = {
      hidUsage: { usagePage: 7, usageMin: 4, usageMax: 40 },
    };
    expect(validateConstraintValue(hid, "10")).toBeNull();
    expect(validateConstraintValue(hid, "2")).toMatch(/≥ 4/);
    expect(validateConstraintValue(hid, "99")).toMatch(/≤ 40/);
  });

  it("enforces non-negative layer indices", () => {
    const layer: SettingConstraint = { layerId: {} };
    expect(validateConstraintValue(layer, "3")).toBeNull();
    expect(validateConstraintValue(layer, "-1")).toMatch(/non-negative/);
  });

  it("checks behavior binding format", () => {
    const behavior: SettingConstraint = { behaviorId: {} };
    expect(validateConstraintValue(behavior, "5,1,2")).toBeNull();
    expect(validateConstraintValue(behavior, "abc")).toMatch(/behaviorId/);
  });
});
