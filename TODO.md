# Production security, privacy, and lifecycle TODO

The current `PassThroughPolicy`, `NoOpPrivacy`, and trusted-header identity mechanism are development placeholders. They provide no production security claim.

- [ ] Authentication: verify workload/user identity instead of trusting request headers.
- [ ] Authorization: bind policy decisions to verified identity and vehicle/resource scope.
- [ ] Purpose and context: authenticate purpose claims and define an enforceable context model.
- [ ] User consent: represent, verify, expire, and revoke consent where applicable.
- [ ] Data minimization: make policy narrow resource, range, sampling, representation, records, and bytes before physical reads.
- [ ] Privacy transformation: implement reviewed redaction/aggregation providers with fail-closed behavior.
- [ ] TLS/mTLS: protect remote bindings and establish deployment-specific certificate lifecycle.
- [ ] Encryption at rest: align AVS storage encryption with performance and recovery requirements.
- [ ] Key management: define hardware-backed storage, rotation, revocation, backup, and recovery.
- [ ] Integrity: authenticate configuration, indexes, archives, payloads, binaries, and updates.
- [ ] Physical storage compromise: document threat model and protection for removable SSD/HDD media.
- [ ] Audit: protect logs from tampering, minimize sensitive fields, export asynchronously, and define alerting.
- [ ] Retention/deletion: enforce policy across SSD, HDD, replicas, backups, indexes, and audit records.
- [ ] ISO/SAE 21434: perform TARA, trace controls, and retain verification evidence.
- [ ] UNECE R155: map the production deployment and operational monitoring to CSMS obligations.
- [ ] Error review: verify every dependency exception maps to a stable, non-sensitive pDAL error.
- [ ] Denial-of-service review: load-test connection, bulk, live, range, record, byte, and queue limits on Raspberry Pi 5 hardware.

Future functionality—not implemented in v1:

- [ ] Add verified VSS/VDM references only where the semantics are genuinely equivalent.
- [ ] Add another `IStorageBackend` only when a real future storage system is selected.
- [ ] Design controlled `DerivedResource`/`DataFunction` execution without arbitrary third-party code.
