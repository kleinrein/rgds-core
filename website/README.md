# Website

The project's landing page, built with [Astro](https://astro.build) and published to GitHub Pages by `.github/workflows/website.yml` when `main` changes.

```sh
npm ci
npm run dev
```

Images come from `docs/images` and `assets/`: `scripts/shots.mjs` prepares them on every dev and build run, so nothing is duplicated here.
