"""State shared by the benchmark tests of one pytest session."""

import dataclasses
import json
import math
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

    def write_json(self, summary: schema.Summary) -> Path:
        """Write the plan, every sample (in timing order) and the summary to run.json."""
        self.out.mkdir(parents=True, exist_ok=True)
        doc = {
            "plan": dataclasses.asdict(self.plan),
            "samples": [{**dataclasses.asdict(s.cell), "seconds": s.seconds} for s in self.samples],
            "summary": dataclasses.asdict(summary),
        }
        (path := self.out / "run.json").write_text(json.dumps(_strict(doc), indent=1) + "\n")
        return path


def _strict(value):
    """NaN (an undefined interval bound) -> null, which strict JSON allows."""
    if isinstance(value, float) and math.isnan(value):
        return None
    if isinstance(value, dict):
        return {k: _strict(v) for k, v in value.items()}
    if isinstance(value, list | tuple):
        return [_strict(v) for v in value]
    return value
