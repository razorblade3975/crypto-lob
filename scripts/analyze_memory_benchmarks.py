#!/usr/bin/env python3
"""
Analyze memory pool benchmark results and generate performance reports.
"""

import json
import sys
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
import numpy as np

def parse_benchmark_json(json_file):
    """Parse Google Benchmark JSON output."""
    with open(json_file, 'r') as f:
        data = json.load(f)
    
    benchmarks = []
    for bench in data['benchmarks']:
        parts = bench['name'].split('/')
        base_name = parts[0]
        
        # Extract allocator type and object type
        if 'StdAllocator' in base_name:
            allocator = 'std::allocator'
        elif 'MemoryPool' in base_name:
            allocator = 'MemoryPool'
        elif 'BoostPool' in base_name:
            allocator = 'Boost.Pool'
        else:
            allocator = 'Unknown'
        
        # Extract object type
        if 'SmallObject' in base_name:
            obj_type = 'Small (32B)'
        elif 'MediumObject' in base_name:
            obj_type = 'Medium (136B)'
        elif 'LargeObject' in base_name:
            obj_type = 'Large (640B)'
        else:
            obj_type = 'Unknown'
        
        # Extract test type
        if 'Sequential' in base_name:
            test_type = 'Sequential'
        elif 'BulkAlloc' in base_name:
            test_type = 'Bulk'
        elif 'RandomPattern' in base_name:
            test_type = 'Random'
        elif 'Multithreaded' in base_name:
            test_type = 'Multithreaded'
        elif 'CacheBehavior' in base_name:
            test_type = 'Cache'
        else:
            test_type = 'Unknown'
        
        benchmarks.append({
            'allocator': allocator,
            'object_type': obj_type,
            'test_type': test_type,
            'time_ns': bench['real_time'],
            'cpu_time_ns': bench['cpu_time'],
            'iterations': bench['iterations'],
            'items_per_second': bench.get('items_per_second', 0),
            'full_name': bench['name']
        })
    
    return pd.DataFrame(benchmarks)

def generate_comparison_plots(df, output_dir):
    """Generate comparison plots for benchmark results."""
    output_dir = Path(output_dir)
    output_dir.mkdir(exist_ok=True)
    
    # Set style
    sns.set_style("whitegrid")
    plt.rcParams['figure.figsize'] = (12, 8)
    
    # 1. Sequential allocation performance by object size
    seq_df = df[df['test_type'] == 'Sequential']
    if not seq_df.empty:
        fig, ax = plt.subplots()
        seq_pivot = seq_df.pivot(index='object_type', columns='allocator', values='time_ns')
        seq_pivot.plot(kind='bar', ax=ax)
        ax.set_ylabel('Time per allocation (ns)')
        ax.set_title('Sequential Allocation Performance by Object Size')
        ax.set_xlabel('Object Size')
        plt.xticks(rotation=0)
        plt.tight_layout()
        plt.savefig(output_dir / 'sequential_performance.png', dpi=300)
        plt.close()
    
    # 2. Multithreaded scaling
    mt_df = df[df['test_type'] == 'Multithreaded']
    if not mt_df.empty:
        # Extract thread count from full_name
        mt_df['threads'] = mt_df['full_name'].str.extract(r'/(\d+)$').astype(int)
        
        fig, ax = plt.subplots()
        for allocator in mt_df['allocator'].unique():
            data = mt_df[mt_df['allocator'] == allocator]
            for obj_type in data['object_type'].unique():
                subset = data[data['object_type'] == obj_type]
                ax.plot(subset['threads'], subset['items_per_second'], 
                       marker='o', label=f'{allocator} - {obj_type}')
        
        ax.set_xlabel('Number of Threads')
        ax.set_ylabel('Operations per Second')
        ax.set_title('Multithreaded Scaling Performance')
        ax.legend()
        plt.tight_layout()
        plt.savefig(output_dir / 'multithreaded_scaling.png', dpi=300)
        plt.close()
    
    # 3. Performance comparison heatmap
    fig, ax = plt.subplots(figsize=(14, 10))
    
    # Create pivot table for heatmap
    pivot = df.pivot_table(
        index=['test_type', 'object_type'], 
        columns='allocator', 
        values='time_ns',
        aggfunc='mean'
    )
    
    # Normalize by row (relative performance)
    pivot_normalized = pivot.div(pivot.min(axis=1), axis=0)
    
    sns.heatmap(pivot_normalized, annot=True, fmt='.2f', cmap='RdYlGn_r', 
                cbar_kws={'label': 'Relative Performance (lower is better)'})
    ax.set_title('Relative Performance Comparison (normalized by best performer per test)')
    plt.tight_layout()
    plt.savefig(output_dir / 'performance_heatmap.png', dpi=300)
    plt.close()
    
    # 4. Box plot for latency distribution
    fig, axes = plt.subplots(2, 2, figsize=(15, 12))
    axes = axes.ravel()
    
    test_types = ['Sequential', 'Random', 'Bulk', 'Cache']
    for idx, test_type in enumerate(test_types):
        if idx < len(axes):
            test_df = df[df['test_type'] == test_type]
            if not test_df.empty:
                test_df.boxplot(column='time_ns', by='allocator', ax=axes[idx])
                axes[idx].set_title(f'{test_type} Allocation Latency')
                axes[idx].set_ylabel('Time (ns)')
                axes[idx].set_xlabel('Allocator')
    
    plt.suptitle('Latency Distribution by Test Type')
    plt.tight_layout()
    plt.savefig(output_dir / 'latency_distribution.png', dpi=300)
    plt.close()

def generate_summary_report(df, output_file):
    """Generate a summary report of benchmark results."""
    with open(output_file, 'w') as f:
        f.write("# Memory Pool Benchmark Summary Report\n\n")
        
        # Overall winner by test type
        f.write("## Best Performer by Test Type\n\n")
        f.write("| Test Type | Object Size | Best Allocator | Time (ns) | Improvement vs std::allocator |\n")
        f.write("|-----------|-------------|----------------|-----------|-------------------------------|\n")
        
        for test_type in df['test_type'].unique():
            for obj_type in df['object_type'].unique():
                subset = df[(df['test_type'] == test_type) & (df['object_type'] == obj_type)]
                if not subset.empty:
                    best = subset.loc[subset['time_ns'].idxmin()]
                    std_time = subset[subset['allocator'] == 'std::allocator']['time_ns'].values
                    if len(std_time) > 0:
                        improvement = (std_time[0] / best['time_ns'] - 1) * 100
                        f.write(f"| {test_type} | {obj_type} | {best['allocator']} | "
                               f"{best['time_ns']:.1f} | {improvement:.1f}% |\n")
        
        f.write("\n## Average Performance Summary\n\n")
        
        # Calculate average performance metrics
        avg_perf = df.groupby('allocator')['time_ns'].agg(['mean', 'std', 'min', 'max'])
        avg_perf = avg_perf.sort_values('mean')
        
        f.write("| Allocator | Mean (ns) | Std Dev | Min (ns) | Max (ns) |\n")
        f.write("|-----------|-----------|---------|----------|----------|\n")
        
        for allocator, row in avg_perf.iterrows():
            f.write(f"| {allocator} | {row['mean']:.1f} | {row['std']:.1f} | "
                   f"{row['min']:.1f} | {row['max']:.1f} |\n")
        
        # Memory pool specific advantages
        f.write("\n## MemoryPool Advantages\n\n")
        
        mp_df = df[df['allocator'] == 'MemoryPool']
        std_df = df[df['allocator'] == 'std::allocator']
        
        if not mp_df.empty and not std_df.empty:
            # Calculate average improvement
            merged = pd.merge(
                mp_df[['test_type', 'object_type', 'time_ns']], 
                std_df[['test_type', 'object_type', 'time_ns']], 
                on=['test_type', 'object_type'],
                suffixes=('_mp', '_std')
            )
            
            merged['improvement'] = (merged['time_ns_std'] / merged['time_ns_mp'] - 1) * 100
            
            f.write("### Performance Improvements over std::allocator\n\n")
            f.write("- Average improvement: {:.1f}%\n".format(merged['improvement'].mean()))
            f.write("- Best improvement: {:.1f}% ({})\n".format(
                merged['improvement'].max(),
                merged.loc[merged['improvement'].idxmax()]['test_type']
            ))
            f.write("- Worst case: {:.1f}%\n".format(merged['improvement'].min()))

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_memory_benchmarks.py <benchmark_results.json> [output_dir]")
        sys.exit(1)
    
    json_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "benchmark_results"
    
    # Parse benchmark results
    df = parse_benchmark_json(json_file)
    
    # Generate plots
    generate_comparison_plots(df, output_dir)
    
    # Generate summary report
    generate_summary_report(df, Path(output_dir) / "summary_report.md")
    
    print(f"Analysis complete. Results saved to {output_dir}/")
    
    # Print quick summary to console
    print("\nQuick Summary:")
    print("=" * 50)
    
    avg_by_allocator = df.groupby('allocator')['time_ns'].mean().sort_values()
    print("\nAverage time per operation by allocator:")
    for allocator, avg_time in avg_by_allocator.items():
        print(f"  {allocator}: {avg_time:.1f} ns")
    
    # Calculate MemoryPool advantage
    if 'MemoryPool' in avg_by_allocator and 'std::allocator' in avg_by_allocator:
        mp_advantage = (avg_by_allocator['std::allocator'] / avg_by_allocator['MemoryPool'] - 1) * 100
        print(f"\nMemoryPool is {mp_advantage:.1f}% faster than std::allocator on average")

if __name__ == "__main__":
    main()