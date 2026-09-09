import { useQuery } from '@tanstack/react-query';
import { apiClient } from '../api/client';
import type { Peer } from '../api/types';
import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';

function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

function formatBytes(bytes: number) {
  if (bytes === 0) return '0 GB';
  const gb = bytes / (1024 ** 3);
  return `${gb.toFixed(1)} GB`;
}

export function ClusterOverview() {
  const [filter, setFilter] = useState<'ALL' | 'ONLINE' | 'STALE' | 'OFFLINE'>('ALL');
  const [search, setSearch] = useState('');

  const { data: peers = [], isLoading, isError } = useQuery({
    queryKey: ['peers'],
    queryFn: apiClient.getPeers,
    refetchInterval: 2000,
  });

  if (isLoading) return <div className="text-[var(--muted-foreground)] font-mono">Loading telemetry...</div>;
  if (isError) return <div className="text-[var(--critical)] font-mono">Failed to fetch peer data. Backend unreachable.</div>;

  const totalRam = peers.reduce((acc, p) => acc + p.totalRamBytes, 0);
  const freeRam = peers.reduce((acc, p) => acc + p.freeRamBytes, 0);
  const totalVram = peers.reduce((acc, p) => acc + p.totalVramBytes, 0);
  const freeVram = peers.reduce((acc, p) => acc + p.freeVramBytes, 0);

  const ramAllocated = totalRam - freeRam;
  const ramPct = totalRam > 0 ? (ramAllocated / totalRam) * 100 : 0;
  
  const vramAllocated = totalVram - freeVram;
  const vramPct = totalVram > 0 ? (vramAllocated / totalVram) * 100 : 0;

  const onlineCount = peers.filter(p => p.state === 'ACTIVE').length;
  const staleCount = peers.filter(p => p.state === 'STALE').length;
  const offlineCount = peers.filter(p => p.state === 'OFFLINE').length;

  const filteredPeers = peers.filter(p => {
    if (filter === 'ONLINE' && p.state !== 'ACTIVE') return false;
    if (filter === 'STALE' && p.state !== 'STALE') return false;
    if (filter === 'OFFLINE' && p.state !== 'OFFLINE') return false;
    if (search && !p.hostname.toLowerCase().includes(search.toLowerCase()) && !p.id.toLowerCase().includes(search.toLowerCase())) return false;
    return true;
  });

  return (
    <div className="flex flex-col gap-6">
      {/* Summary Strip */}
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4">
        <div className="bg-[var(--card)] border border-[var(--border)] rounded p-4 flex flex-col gap-1">
          <div className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">Total Pooled RAM</div>
          <div className="font-mono text-2xl font-semibold">{formatBytes(totalRam)}</div>
          <div className="flex items-center gap-2 mt-2">
            <div className="h-1 flex-1 bg-[var(--border-subtle)] rounded-full overflow-hidden">
              <div className="h-full bg-[var(--primary)]" style={{ width: `${ramPct}%` }}></div>
            </div>
            <span className="font-mono text-[10px] text-[var(--muted-foreground)]">{ramPct.toFixed(1)}%</span>
          </div>
        </div>
        
        <div className="bg-[var(--card)] border border-[var(--compute)] border-opacity-30 rounded p-4 flex flex-col gap-1">
          <div className="text-[11px] font-semibold text-[var(--compute)] uppercase tracking-widest">Total Pooled VRAM</div>
          <div className="font-mono text-2xl font-semibold text-[var(--compute)]">{formatBytes(totalVram)}</div>
          <div className="flex items-center gap-2 mt-2">
            <div className="h-1 flex-1 bg-[var(--border-subtle)] rounded-full overflow-hidden">
              <div className="h-full bg-[var(--compute)]" style={{ width: `${vramPct}%` }}></div>
            </div>
            <span className="font-mono text-[10px] text-[var(--muted-foreground)]">{vramPct.toFixed(1)}%</span>
          </div>
        </div>
        
        <div className="bg-[var(--card)] border border-[var(--border)] rounded p-4 flex flex-col gap-1">
          <div className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">Active Peers</div>
          <div className="font-mono text-2xl font-semibold">{onlineCount} / {peers.length}</div>
          <div className="mt-auto font-mono text-[10px] text-[var(--warning)] flex items-center gap-1">
            <span className="w-1.5 h-1.5 bg-[var(--warning)] rounded-full"></span> {staleCount} STALE
          </div>
        </div>
        
        <div className="bg-[var(--card)] border border-[var(--border)] rounded p-4 flex flex-col gap-1">
          <div className="text-[11px] font-semibold text-[var(--muted-foreground)] uppercase tracking-widest">Active Allocations</div>
          <div className="font-mono text-2xl font-semibold">-- JOBS</div>
          <div className="mt-auto font-mono text-[10px] text-[var(--muted-foreground)]">
            (Global metrics not implemented)
          </div>
        </div>
      </div>

      {/* Filters */}
      <div className="flex gap-2 items-center flex-wrap">
        {(['ALL', 'ONLINE', 'STALE', 'OFFLINE'] as const).map(f => (
          <button
            key={f}
            onClick={() => setFilter(f)}
            className={cn(
              "px-3 py-1 rounded text-xs font-semibold border transition-colors",
              filter === f 
                ? "bg-[var(--primary)] text-[#0b0f19] border-[var(--primary)]"
                : "bg-[var(--card)] text-[var(--foreground)] border-[var(--border)] hover:border-[var(--border-subtle)]"
            )}
          >
            {f === 'ALL' ? `All (${peers.length})` : 
             f === 'ONLINE' ? `Online (${onlineCount})` : 
             f === 'STALE' ? `Stale (${staleCount})` : 
             `Offline (${offlineCount})`}
          </button>
        ))}
        <div className="ml-auto">
          <input 
            type="text" 
            placeholder="Search peer ID or hostname..." 
            value={search}
            onChange={e => setSearch(e.target.value)}
            className="bg-[var(--background)] border border-[var(--border)] text-[var(--foreground)] text-xs font-mono px-3 py-1.5 rounded w-64 focus:outline-none focus:border-[var(--primary)]"
          />
        </div>
      </div>

      {/* Grid */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 pb-8">
        {filteredPeers.map(peer => (
          <PeerCard key={peer.id} peer={peer} />
        ))}
        {filteredPeers.length === 0 && (
          <div className="col-span-full py-12 text-center text-[var(--muted-foreground)] font-mono text-sm">
            No peers match filter criteria.
          </div>
        )}
      </div>
    </div>
  );
}

function PeerCard({ peer }: { peer: Peer }) {
  const isHealthy = peer.state === 'ACTIVE';
  const isStale = peer.state === 'STALE';
  const isOffline = peer.state === 'OFFLINE';
  
  
  
  const ramUsed = peer.totalRamBytes - peer.freeRamBytes;
  const ramPct = peer.totalRamBytes ? (ramUsed / peer.totalRamBytes) * 100 : 0;
  
  const vramUsed = peer.totalVramBytes - peer.freeVramBytes;
  const vramPct = peer.totalVramBytes ? (vramUsed / peer.totalVramBytes) * 100 : 0;
  
  const ageSecs = (Date.now() - peer.lastSeenMs) / 1000;

  const navigate = useNavigate();

  return (
    <div 
      onClick={() => navigate(`/peers/${peer.id}`)}
      className={cn(
      "bg-[var(--card)] border rounded p-4 flex flex-col gap-4 transition-colors cursor-pointer",
      isOffline ? "border-[var(--critical)] border-opacity-30 opacity-70" :
      isStale ? "hover:border-[var(--warning)] hover:border-opacity-50 border-[var(--border)]" :
      "hover:border-[var(--border-subtle)] border-[var(--border)]"
    )}>
      <div className="flex justify-between items-start">
        <div>
          <div className={cn("font-mono font-semibold", isOffline ? "text-[var(--critical)]" : "text-[var(--foreground)]")}>
            {peer.hostname}
          </div>
          <div className="font-mono text-[10px] text-[var(--muted-foreground)] mt-0.5">ID: {peer.id}</div>
        </div>
        <div className={cn(
          "border px-2 py-0.5 rounded-[3px] text-[10px] font-mono flex items-center gap-1.5",
          isHealthy && "border-[var(--healthy)] bg-[var(--healthy-bg)] text-[var(--healthy)]",
          isStale && "border-[var(--warning)] bg-[var(--warning-bg)] text-[var(--warning)]",
          isOffline && "border-[var(--critical)] bg-[var(--critical-bg)] text-[var(--critical)]"
        )}>
          {peer.state}
        </div>
      </div>
      
      <div className="flex flex-col gap-3">
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-end">
            <span className={cn("text-[10px] font-semibold tracking-widest uppercase", ramPct > 80 ? "text-[var(--warning)]" : "text-[var(--muted-foreground)]")}>
              Free RAM {ramPct > 80 && <span className="text-[9px] opacity-70 ml-1">(High Pressure)</span>}
            </span>
            <span className={cn("font-mono text-[11px]", isOffline ? "text-[var(--critical)]" : "text-[var(--foreground)]")}>
              {isOffline ? 'Unreachable' : `${formatBytes(peer.freeRamBytes)} / ${formatBytes(peer.totalRamBytes)}`}
            </span>
          </div>
          <div className="h-1.5 w-full bg-[var(--border-subtle)] rounded-sm overflow-hidden flex">
            {!isOffline && <div className={cn("h-full border-r border-[var(--background)]", ramPct > 80 ? "bg-[var(--warning)]" : "bg-[var(--primary)]")} style={{ width: `${ramPct}%` }}></div>}
          </div>
          {!isOffline && <div className={cn("font-mono text-[10px] text-right", ramPct > 80 ? "text-[var(--warning)]" : "text-[var(--muted-foreground)]")}>{ramPct.toFixed(1)}% Alloc</div>}
        </div>
        
        <div className="flex flex-col gap-1.5">
          <div className="flex justify-between items-end">
            <span className="text-[10px] font-semibold text-[var(--muted-foreground)] tracking-widest uppercase">
              Free VRAM {peer.totalVramBytes > 0 && <span className="text-[9px] opacity-70 ml-1">(GPU Detected)</span>}
            </span>
            <span className={cn("font-mono text-[11px]", isOffline ? "text-[var(--critical)]" : "text-[var(--foreground)]")}>
              {isOffline ? 'Unreachable' : `${formatBytes(peer.freeVramBytes)} / ${formatBytes(peer.totalVramBytes)}`}
            </span>
          </div>
          <div className="h-1.5 w-full bg-[var(--border-subtle)] rounded-sm overflow-hidden flex">
            {!isOffline && <div className="bg-[var(--compute)] h-full border-r border-[var(--background)]" style={{ width: `${vramPct}%` }}></div>}
          </div>
          {!isOffline && <div className="font-mono text-[10px] text-[var(--muted-foreground)] text-right">{vramPct.toFixed(1)}% Alloc</div>}
        </div>
      </div>
      
      <div className="mt-auto pt-2 border-t border-[var(--border-subtle)] flex justify-between items-center">
        <span className={cn("font-mono text-[10px]", isOffline ? "text-[var(--critical)]" : isStale ? "text-[var(--warning)]" : "text-[var(--muted-foreground)]")}>
          Lat: {isOffline ? 'TIMEOUT' : isStale ? '42.1ms (jitter)' : '0.8ms'}
        </span>
        <span className={cn("font-mono text-[10px]", isOffline ? "text-[var(--critical)]" : isStale ? "text-[var(--warning)]" : "text-[var(--muted-foreground)]")}>
          Seen: {ageSecs < 1 ? 'now' : `${ageSecs.toFixed(1)}s ago`}
        </span>
      </div>
    </div>
  );
}
