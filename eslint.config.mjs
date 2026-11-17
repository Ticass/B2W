import js from "@eslint/js";

export default [
  {
    files: ["catalog/**/*.js"],
    ...js.configs.recommended,
    languageOptions: {
      sourceType: "script",
      globals: {
        document: "readonly",
      },
    },
  },
];
