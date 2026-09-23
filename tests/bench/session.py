"""State shared by the benchmark tests of one pytest session."""

from __future__ import annotations

import random
from dataclasses import dataclass, field
from pathlib import Path

import runner
import schema
import stats


@dataclass
class BenchSession:
    plan: schema.Plan
    env: runner.Environment
    out: Path
    rng: random.Random
    samples: list[schema.Sample] = field(default_factory=list)

    def run(self) -> schema.Run:
        return schema.Run(plan=self.plan, samples=tuple(self.samples))

    def summary(self) -> schema.Summary:
        return stats.summarize(self.run())
