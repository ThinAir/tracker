subdir('ipc')

test_runner = find_program('test-runner.sh')

functional_tests = [
  '01-insertion',
  '02-sparql-bugs',
  '03-fts-functions',
  '04-group-concat',
  '05-coalesce',
  '06-distance',
  '07-graph',
  '08-unique-insertions',
  '09-concurrent-query',
  '10-sqlite-misused',
  '11-sqlite-batch-misused',
  '12-transactions',
  '13-threaded-store',
  '14-signals',
  '15-statistics',
  '16-collation',
  '17-ontology-changes',
  '200-backup-restore',
  '300-miner-basic-ops',
  '301-miner-resource-removal',
  '310-fts-indexing',
  '400-extractor-metadata',
  '410-extractor-decorator',
  '500-writeback',
  '501-writeback-details',
  '600-applications-camera',
  '601-applications-sync',
]

foreach t: functional_tests
  test('functional-' + t, test_runner,
    args: './' + t + '.py',
    workdir: meson.current_source_dir(),
    # FIXME: these tests are all too slow
    timeout: 180)
endforeach
