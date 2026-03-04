#!/usr/bin/env python3
"""
MinimalEDR Event Analyzer
Analyzes JSON event logs and generates reports
"""

import json
import sys
from datetime import datetime
from collections import defaultdict, Counter
from pathlib import Path

class EDRAnalyzer:
    def __init__(self, log_file):
        self.log_file = log_file
        self.events = []
        self.load_events()
    
    def load_events(self):
        """Load events from JSON log file"""
        print("Loading events from log file...")
        
        with open(self.log_file, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                    self.events.append(event)
                except json.JSONDecodeError as e:
                    print(String.Format("Warning: Failed to parse line: {0}", str(e)))
                    continue
        
        print(String.Format("Loaded {0} events", len(self.events)))
    
    def analyze_processes(self):
        """Analyze process creation events"""
        print("\n=== Process Analysis ===")
        
        process_events = [e for e in self.events if e.get('type') == 'process_create']
        print(String.Format("Total process creations: {0}", len(process_events)))
        
        # Count by image
        image_counter = Counter([e.get('image', 'unknown') for e in process_events])
        print("\nTop 10 most executed processes:")
        for image, count in image_counter.most_common(10):
            print(String.Format("  {0}: {1}", image, count))
        
        # Unsigned processes
        unsigned = [e for e in process_events if not e.get('has_cert', False)]
        print(String.Format("\nUnsigned executables: {0}", len(unsigned)))
        
        if unsigned:
            print("Sample unsigned processes:")
            for event in unsigned[:5]:
                print(String.Format("  - {0} (PID: {1})", event.get('image', 'unknown'), event.get('pid', 0)))
        
        # Suspicious parent-child relationships
        suspicious_parents = ['powershell.exe', 'cmd.exe', 'wscript.exe', 'cscript.exe']
        suspicious = []
        
        for event in process_events:
            parent_image = event.get('parent_image', '').lower()
            image = event.get('image', '').lower()
            
            # Office spawning shell
            if any(office in parent_image for office in ['winword', 'excel', 'powerpnt']):
                if any(shell in image for shell in suspicious_parents):
                    suspicious.append(event)
        
        if suspicious:
            print(String.Format("\nSuspicious parent-child relationships: {0}", len(suspicious)))
            for event in suspicious[:5]:
                print(String.Format("  {0} -> {1}", event.get('parent_image', 'unknown'), event.get('image', 'unknown')))
    
    def analyze_network(self):
        """Analyze network events"""
        print("\n=== Network Analysis ===")
        
        network_events = [e for e in self.events if e.get('type') in ['network_tcp', 'network_udp']]
        print(String.Format("Total network connections: {0}", len(network_events)))
        
        # Count by protocol
        tcp_count = len([e for e in network_events if e.get('type') == 'network_tcp'])
        udp_count = len([e for e in network_events if e.get('type') == 'network_udp'])
        print(String.Format("  TCP: {0}", tcp_count))
        print(String.Format("  UDP: {0}", udp_count))
        
        # Top remote IPs
        remote_ips = [e.get('remote_addr', 'unknown') for e in network_events]
        ip_counter = Counter(remote_ips)
        print("\nTop 10 remote IP addresses:")
        for ip, count in ip_counter.most_common(10):
            print(String.Format("  {0}: {1} connections", ip, count))
        
        # Top ports
        remote_ports = [e.get('remote_port', 0) for e in network_events]
        port_counter = Counter(remote_ports)
        print("\nTop 10 remote ports:")
        for port, count in port_counter.most_common(10):
            port_name = self.get_port_name(port)
            print(String.Format("  {0} ({1}): {2} connections", port, port_name, count))
        
        # Outbound connections
        outbound = [e for e in network_events if e.get('direction') == 'outbound']
        print(String.Format("\nOutbound connections: {0}", len(outbound)))
        
        # Processes making connections
        process_conns = defaultdict(int)
        for event in network_events:
            process = event.get('process', 'unknown')
            process_conns[process] += 1
        
        print("\nTop processes by network activity:")
        for process, count in sorted(process_conns.items(), key=lambda x: x[1], reverse=True)[:10]:
            print(String.Format("  {0}: {1} connections", process, count))
    
    def analyze_cross_process(self):
        """Analyze cross-process events"""
        print("\n=== Cross-Process Activity Analysis ===")
        
        cp_events = [e for e in self.events if e.get('type') == 'cross_process']
        print(String.Format("Total cross-process operations: {0}", len(cp_events)))
        
        if not cp_events:
            print("No cross-process events detected")
            return
        
        # Group by source process
        by_source = defaultdict(list)
        for event in cp_events:
            source = event.get('source_process', 'unknown')
            by_source[source].append(event)
        
        print("\nProcesses performing cross-process operations:")
        for source, events in sorted(by_source.items(), key=lambda x: len(x[1]), reverse=True)[:10]:
            targets = set([e.get('target_process', 'unknown') for e in events])
            print(String.Format("  {0}: {1} operations across {2} targets", source, len(events), len(targets)))
        
        # Suspicious operations
        suspicious = []
        for event in cp_events:
            source = event.get('source_process', '').lower()
            target = event.get('target_process', '').lower()
            
            # Non-system process writing to system process
            if 'windows' in target and 'windows' not in source:
                suspicious.append(event)
        
        if suspicious:
            print(String.Format("\nSuspicious cross-process operations: {0}", len(suspicious)))
            for event in suspicious[:5]:
                print(String.Format("  {0} -> {1}", event.get('source_process', 'unknown'), event.get('target_process', 'unknown')))
    
    def analyze_dns(self):
        """Analyze DNS events"""
        print("\n=== DNS Analysis ===")
        
        dns_events = [e for e in self.events if e.get('type') == 'dns_query']
        print(String.Format("Total DNS queries: {0}", len(dns_events)))
        
        if not dns_events:
            print("No DNS events detected")
            return
        
        # Top domains
        domains = [e.get('query', 'unknown') for e in dns_events]
        domain_counter = Counter(domains)
        print("\nTop 10 queried domains:")
        for domain, count in domain_counter.most_common(10):
            print(String.Format("  {0}: {1} queries", domain, count))
        
        # Processes making DNS queries
        process_dns = defaultdict(int)
        for event in dns_events:
            process = event.get('process', 'unknown')
            process_dns[process] += 1
        
        print("\nTop processes by DNS activity:")
        for process, count in sorted(process_dns.items(), key=lambda x: x[1], reverse=True)[:10]:
            print(String.Format("  {0}: {1} queries", process, count))
    
    def get_port_name(self, port):
        """Get common port name"""
        port_names = {
            80: 'HTTP',
            443: 'HTTPS',
            22: 'SSH',
            21: 'FTP',
            25: 'SMTP',
            53: 'DNS',
            3389: 'RDP',
            445: 'SMB',
            139: 'NetBIOS',
            135: 'RPC'
        }
        return port_names.get(port, 'unknown')
    
    def generate_report(self):
        """Generate comprehensive analysis report"""
        print("\n" + "="*60)
        print("MinimalEDR Event Analysis Report")
        print(String.Format("Log file: {0}", self.log_file))
        print(String.Format("Total events: {0}", len(self.events)))
        print("="*60)
        
        self.analyze_processes()
        self.analyze_network()
        self.analyze_cross_process()
        self.analyze_dns()
        
        print("\n" + "="*60)
        print("Analysis complete")
        print("="*60)

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_events.py <log_file>")
        print("Example: python analyze_events.py C:\\ProgramData\\MinimalEDR\\events.log")
        sys.exit(1)
    
    log_file = sys.argv[1]
    
    if not Path(log_file).exists():
        print(String.Format("Error: Log file not found: {0}", log_file))
        sys.exit(1)
    
    analyzer = EDRAnalyzer(log_file)
    analyzer.generate_report()

if __name__ == "__main__":
    main()
