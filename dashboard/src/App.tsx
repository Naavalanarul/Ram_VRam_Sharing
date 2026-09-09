import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { Layout } from './components/layout/Layout';
import { ClusterOverview } from './pages/ClusterOverview';
import { PeerDetail } from './pages/PeerDetail';
import { GpuJobs } from './pages/GpuJobs';
import { Settings } from './pages/Settings';
import { ActivityLog } from './pages/ActivityLog';

function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<Layout />}>
          <Route index element={<ClusterOverview />} />
          <Route path="peers" element={
            <div className="text-[var(--muted-foreground)] font-mono">Select a peer from the Cluster Overview to view details.</div>
          } />
          <Route path="peers/:id" element={<PeerDetail />} />
          <Route path="gpu" element={<GpuJobs />} />
          <Route path="settings" element={<Settings />} />
          <Route path="logs" element={<ActivityLog />} />
        </Route>
      </Routes>
    </BrowserRouter>
  );
}

export default App;
