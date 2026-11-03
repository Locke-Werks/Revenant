// The test runner is node's built-in one and @types/node is not a dependency,
// so the few names the tests use are declared here.
declare module "node:test" {
  export function test(name: string, fn: () => void | Promise<void>): void;
}
declare module "node:assert/strict" {
  const assert: {
    equal(actual: unknown, expected: unknown, message?: string): void;
    deepEqual(actual: unknown, expected: unknown): void;
    match(actual: string, re: RegExp): void;
    rejects(p: Promise<unknown>, expected?: RegExp | { message?: string | RegExp }): Promise<void>;
    ok(value: unknown, message?: string): void;
  };
  export default assert;
}
declare module "node:fs" {
  export function readFileSync(path: string): Uint8Array;
  export function readFileSync(path: string, encoding: "utf8"): string;
  export function readdirSync(path: string): string[];
}
