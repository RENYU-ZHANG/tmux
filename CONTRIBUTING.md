# Tmux fork maintenance

Maintain the fork's local changes as an independently selectable patch series
over an agreed upstream baseline.

## Patch isolation

Maintain native changes as explicit shared-foundation patches and independent
feature patches. Dependencies run downward to declared shared interfaces, not
between sibling features. Shared layers contain genuinely reusable mechanisms;
feature policy stays in its owning feature. Sibling feature patches leave each
other's changed source lines untouched.

When a new patch would violate this isolation, refactor the affected boundary
before integrating it, extracting shared mechanisms where needed. Keep this
incremental and scoped to the new patch's actual needs; adoption of this style
does not require a blanket rewrite of the existing fork.

## Commit boundaries

Keep each independently selectable patch in one coherent commit in the maintained
patch series, including its tests and documentation. Keep shared prerequisites
in separate commits and identify them in dependent commit messages. Temporary
development fixups may be consolidated before publication; rewriting published
history requires explicit agreement.

## Verification

Verify shared foundations without upper features. Each feature must apply, build
and pass its behavior tests on the agreed upstream baseline plus its declared
shared prerequisites, without sibling features. Verify the combined profile and
that removing a feature preserves the remaining features. Separate files,
non-overlapping changes and linear Git history alone do not prove independence.
