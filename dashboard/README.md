# MemInfo Dashboard

## Architecture Decision: Polling over WebSockets

This dashboard uses **HTTP polling via React Query** (refetchInterval: 2s for Cluster Overview, 3s for Activity Log) instead of WebSockets for real-time updates.

### Rationale

- **Operational simplicity**: No WebSocket server implementation needed in the Go/Rust/C++ backend daemons
- **Reliability**: HTTP polling works through proxies, load balancers, and firewalls without additional configuration
- **Statelessness**: Daemons remain fully stateless w.r.t. dashboard clients; no connection lifecycle management
- **Sufficient latency**: 2–3s polling interval is well within human-perceptible bounds for cluster telemetry dashboards
- **Testability**: Easier to mock/test with standard HTTP tooling (MSW, curl, etc.)

### Trade-offs Acknowledged

- **Higher request volume**: ~15–30 req/s per open dashboard tab vs. 1 persistent WS connection
- **Stale reads**: Data may be up to `refetchInterval` old; acceptable for observability, not for control loops
- **No push notifications**: Users must refresh or wait for next poll to see state changes (e.g., peer join/leave)

### When to Reconsider

- Dashboard scales to >50 concurrent tabs with tight polling
- Backend adds a dedicated event bus (e.g., Redis Pub/Sub) that can fan out to WebSocket gateway
- Real-time collaboration features added (multi-user cursor presence, etc.)

---

## Development

### Prerequisites

- Node.js 18+
- npm or pnpm

### Install

```bash
cd dashboard
npm install
```

### Dev Server

```bash
npm run dev
```

Runs on `http://localhost:5173` with MSW (Mock Service Worker) for API mocking.

### Build

```bash
npm run build
```

Outputs to `dist/` for static hosting.

### Lint

```bash
npm run lint
```

---

## Project Structure

```
dashboard/
├── src/
│   ├── api/
│   │   ├── client.ts      # HTTP API client (fetcher + endpoints)
│   │   └── types.ts       # TypeScript types for API responses
│   ├── components/
│   │   └── layout/        # Sidebar, Topbar, Layout wrapper
│   ├── pages/
│   │   ├── ClusterOverview.tsx   # Main cluster telemetry view (polls /peers every 2s)
│   │   ├── PeerDetail.tsx        # Per-peer detail view
│   │   ├── ActivityLog.tsx       # Log viewer (polls /logs every 3s)
│   │   ├── GpuJobs.tsx           # GPU job queue view
│   │   └── Settings.tsx          # Configuration editor
│   ├── mocks/
│   │   ├── handlers.ts     # MSW request handlers for dev
│   │   └── browser.ts      # MSW worker registration
│   ├── App.tsx             # Router + providers
│   └── main.tsx            # Entry point
├── DESIGN.md               # Design system specification
├── package.json
├── tsconfig.json
└── vite.config.ts
```

---

## API Contract (Expected Backend Endpoints)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/peers` | List all discovered peers (for ClusterOverview) |
| GET | `/peers/:id` | Peer detail (for PeerDetail) |
| GET | `/peers/:id/history` | Time-series history for charts |
| GET | `/status/local` | Local daemon status (for GET_LOCAL_STATUS) |
| POST | `/pool/join` | Join resource pool (JOIN_POOL) |
| POST | `/pool/leave` | Leave resource pool (LEAVE_POOL) |
| GET | `/gpu/jobs` | List GPU jobs |
| GET | `/logs` | Query logs with severity/search/pagination |
| GET | `/config` | Get daemon configuration |
| POST | `/config` | Update daemon configuration |

All endpoints return JSON. Errors use standard HTTP status codes with `{ message: string }` body.

---

## Mock Service Worker (MSW)

In development, MSW intercepts fetch requests to `/api/*` and returns realistic mock data. This allows frontend development without running the full backend stack.

To customize mock data, edit `src/mocks/handlers.ts`.

---

## Design System

See [DESIGN.md](./DESIGN.md) for color palette, typography, and component conventions.