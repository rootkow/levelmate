import { defineConfig } from "vite";

export default defineConfig({
  root: "src/demo",
  build: {
    outDir: "../../dist/demo",
    emptyOutDir: true,
  },
});
