import { defineConfig } from 'astro/config';

// With a custom domain: set site to it, remove base and add public/CNAME.
export default defineConfig({
  site: 'https://kleinrein.github.io',
  base: '/rgds-core',
  devToolbar: { enabled: false },
});
