// The benchmark's library of host.call() (bench.c): a call the application
// answers with what it was given.
import host from 'zebkit:host';

export const viaHost = (value) => host.call('echo', value);
