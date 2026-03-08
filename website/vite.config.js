import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import fs from 'fs'
import path from 'path'

const versionJson = JSON.parse(
  fs.readFileSync(path.resolve(__dirname, 'public/api/version.json'), 'utf-8')
)

// https://vite.dev/config/
export default defineConfig({
  plugins: [vue()],
  define: {
    __APP_VERSION__: JSON.stringify(versionJson.latest_version),
    __APP_DOWNLOAD_URL__: JSON.stringify(versionJson.download_url),
    __APP_DOWNLOAD_URL_WIN__: JSON.stringify(
      versionJson.platforms?.windows?.download_url ?? versionJson.download_url
    ),
    __APP_DOWNLOAD_URL_MAC__: JSON.stringify(
      versionJson.platforms?.macos?.download_url ?? ''
    ),
  },
})
