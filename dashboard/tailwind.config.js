/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        background: 'var(--background)',
        card: 'var(--card)',
        border: 'var(--border)',
        'border-subtle': 'var(--border-subtle)',
        foreground: 'var(--foreground)',
        'muted-foreground': 'var(--muted-foreground)',
        primary: 'var(--primary)',
        healthy: 'var(--healthy)',
        'healthy-bg': 'var(--healthy-bg)',
        warning: 'var(--warning)',
        'warning-bg': 'var(--warning-bg)',
        critical: 'var(--critical)',
        'critical-bg': 'var(--critical-bg)',
        compute: 'var(--compute)',
        'compute-bg': 'var(--compute-bg)',
      },
      fontFamily: {
        mono: ['var(--font-mono)', 'monospace'],
        sans: ['var(--font-sans)', 'sans-serif'],
      },
    },
  },
  plugins: [],
}
