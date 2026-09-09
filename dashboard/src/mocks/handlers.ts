import { http, HttpResponse } from 'msw';
import type { Peer, PeerDetail, TimeSeriesPoint, GpuJob, DaemonConfig, LogEntry, LocalStatus } from '../api/types';

const now = Date.now();

const mockPeers: Peer[] = [
  {
    id: '0x8f2a9c10',
    hostname: 'rig-node-01.lan',
    address: '192.168.1.101',
    freeRamBytes: 42.6 * 1024 ** 3,
    totalRamBytes: 64 * 1024 ** 3,
    freeVramBytes: 18.2 * 1024 ** 3,
    totalVramBytes: 24 * 1024 ** 3,
    memoryPort: 9200,
    gpuPort: 9201,
    state: 'ACTIVE',
    lastSeenMs: now - 200,
  },
  {
    id: '0x11b43e88',
    hostname: 'compute-box-03.lan',
    address: '192.168.1.103',
    freeRamBytes: 88.0 * 1024 ** 3,
    totalRamBytes: 128 * 1024 ** 3,
    freeVramBytes: 36.4 * 1024 ** 3,
    totalVramBytes: 48 * 1024 ** 3,
    memoryPort: 9200,
    gpuPort: 9201,
    state: 'ACTIVE',
    lastSeenMs: now - 1100,
  },
  {
    id: '0x99dc01ff',
    hostname: 'tensor-mini-02.lan',
    address: '192.168.1.102',
    freeRamBytes: 6.2 * 1024 ** 3,
    totalRamBytes: 32 * 1024 ** 3,
    freeVramBytes: 2.1 * 1024 ** 3,
    totalVramBytes: 16 * 1024 ** 3,
    memoryPort: 9200,
    gpuPort: 9201,
    state: 'STALE',
    lastSeenMs: now - 18400,
  },
  {
    id: '0x3d7e82ba',
    hostname: 'render-node-04.lan',
    address: '192.168.1.104',
    freeRamBytes: 0,
    totalRamBytes: 64 * 1024 ** 3,
    freeVramBytes: 0,
    totalVramBytes: 24 * 1024 ** 3,
    memoryPort: 9200,
    gpuPort: 9201,
    state: 'OFFLINE',
    lastSeenMs: now - 14 * 60 * 1000,
  }
];

let mockConfig: DaemonConfig = {
  slabPercentage: 80,
  safetyFloorMb: 1024,
  peerTimeoutMs: 10000,
  cacheSizeMb: 4096,
};

let localStatus: LocalStatus = {
  localNodeId: '0xaa4277b1',
  localHostname: 'dev-mac-m2max.lan',
  poolJoined: true,
};

export const handlers = [
  http.get('/api/peers', () => {
    return HttpResponse.json(mockPeers);
  }),

  http.get('/api/peers/:id', ({ params }) => {
    const peer = mockPeers.find(p => p.id === params.id);
    if (!peer) return new HttpResponse(null, { status: 404 });
    const detail: PeerDetail = {
      ...peer,
      hostedAllocations: [
        { id: 'alloc-1', sizeBytes: 1.5 * 1024**3, ownerPeerId: '0xaa4277b1', ageMs: 120000 },
        { id: 'alloc-2', sizeBytes: 512 * 1024**2, ownerPeerId: '0xaa4277b1', ageMs: 45000 },
      ],
      placedAllocations: []
    };
    return HttpResponse.json(detail);
  }),

  http.get('/api/peers/:id/history', () => {
    // Generate some fake history
    const history: TimeSeriesPoint[] = [];
    let t = now - 60000 * 30; // 30 mins ago
    for (let i = 0; i < 30; i++) {
      history.push({
        timestamp: t,
        ramUsageBytes: (10 + Math.random() * 5) * 1024**3,
        vramUsageBytes: (4 + Math.random() * 2) * 1024**3,
      });
      t += 60000;
    }
    return HttpResponse.json(history);
  }),

  http.get('/api/gpu/jobs', () => {
    const jobs: GpuJob[] = [
      { id: 'job-1', clientNodeId: '0xaa4277b1', gpuNodeId: '0x8f2a9c10', callType: 'cudaMemcpy', status: 'COMPLETE', durationMs: 42, submittedAtMs: now - 5000 },
      { id: 'job-2', clientNodeId: '0xaa4277b1', gpuNodeId: '0x11b43e88', callType: 'cudaLaunchKernel', status: 'RUNNING', durationMs: null, submittedAtMs: now - 1200 },
      { id: 'job-3', clientNodeId: '0x99dc01ff', gpuNodeId: '0x11b43e88', callType: 'cudaMalloc', status: 'ERROR', durationMs: 15, submittedAtMs: now - 15000 },
    ];
    return HttpResponse.json(jobs);
  }),

  http.get('/api/config', () => {
    return HttpResponse.json(mockConfig);
  }),

  http.post('/api/config', async ({ request }) => {
    const body = await request.json();
    mockConfig = { ...mockConfig, ...(body as Partial<DaemonConfig>) };
    return HttpResponse.json(mockConfig);
  }),

  http.get('/api/status/local', () => {
    return HttpResponse.json(localStatus);
  }),

  http.post('/api/pool/join', () => {
    localStatus.poolJoined = true;
    return HttpResponse.json({ success: true });
  }),

  http.post('/api/pool/leave', () => {
    localStatus.poolJoined = false;
    return HttpResponse.json({ success: true });
  }),

  http.get('/api/logs', () => {
    const logs: LogEntry[] = [
      { id: 'l1', timestamp: now - 1000, severity: 'INFO', message: 'Peer discovered: rig-node-01.lan', module: 'discoveryd' },
      { id: 'l2', timestamp: now - 5000, severity: 'WARN', message: 'High memory pressure on tensor-mini-02.lan', module: 'memoryd' },
      { id: 'l3', timestamp: now - 15000, severity: 'ERROR', message: 'GPU job job-3 failed: CUDA_ERROR_OUT_OF_MEMORY', module: 'gpud' },
      { id: 'l4', timestamp: now - 20000, severity: 'INFO', message: 'Daemon started', module: 'core' },
    ];
    return HttpResponse.json(logs);
  }),
];
