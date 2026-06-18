import { cp, mkdir } from "node:fs/promises";
import { resolve } from "node:path";
import { build } from "vite";

const root = process.cwd();
const output = resolve(root, "dist/extension");

await build({
  configFile: false,
  publicDir: false,
  build: {
    outDir: output,
    emptyOutDir: true,
    lib: {
      entry: resolve(root, "src/extension/content/content.ts"),
      formats: ["iife"],
      name: "LevelMateContent",
      fileName: () => "content/content.js",
    },
    rollupOptions: {
      output: { inlineDynamicImports: true },
    },
  },
});

await mkdir(output, { recursive: true });
await cp(resolve(root, "src/extension/manifest.json"), resolve(output, "manifest.json"));

await build({
  configFile: false,
  root: resolve(root, "src/extension/popup"),
  base: "./",
  publicDir: false,
  build: {
    outDir: resolve(output, "popup"),
    emptyOutDir: false,
    rollupOptions: {
      input: resolve(root, "src/extension/popup/popup.html"),
    },
  },
});
