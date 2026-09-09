export type PeerState = 'ACTIVE' | 'STALE' | 'OFFLINE';

export interface Peer {
  id: string;
  hostname: string;
  address: string;
  freeRamBytes: number;
  totalRamBytes: number;
  freeVramBytes: number;
  totalVramBytes: number;
  memoryPort: number;
  gpuPort: number;
  state: PeerState;
  lastSeenMs: number;
}

export interface Allocation {
  id: string;
  sizeBytes: number;
  ownerPeerId: string;
  ageMs: number;
}

export interface PeerDetail extends Peer {
  hostedAllocations: Allocation[];
  placedAllocations: Allocation[];
}

export interface TimeSeriesPoint {
  timestamp: number;
  ramUsageBytes: number;
  vramUsageBytes: number;
}

export type GpuJobStatus = 'PENDING' | 'RUNNING' | 'COMPLETE' | 'ERROR';

export interface GpuJob {
  id: string;
  clientNodeId: string;
  gpuNodeId: string;
  callType: string;
  status: GpuJobStatus;
  durationMs: number | null;
  submittedAtMs: number;
}

export interface DaemonConfig {
  slabPercentage: number;
  safetyFloorMb: number;
  peerTimeoutMs: number;
  cacheSizeMb: number;
}

export type LogSeverity = 'INFO' | 'WARN' | 'ERROR';

export interface LogEntry {
  id: string;
  timestamp: number;
  severity: LogSeverity;
  message: string;
  module: string;
}

export type WsEvent =
  | { type: 'peer_joined'; payload: Peer }
  | { type: 'peer_left'; payload: { id: string } }
  | { type: 'stats_update'; payload: { peerId: string; freeRamBytes: number; freeVramBytes: number } }
  | { type: 'gpu_job_update'; payload: GpuJob }
  | { type: 'log_entry'; payload: LogEntry };

export interface LocalStatus {
  localNodeId: string;
  localHostname: string;
  poolJoined: boolean;
}
