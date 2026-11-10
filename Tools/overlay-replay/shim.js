// The game's object in the overlay: `on`/`off` keep callbacks, anything else is a call to the game, written down.
(function () {
  const callbacks = {};
  const calls = [];
  const errors = [];
  window.__calls = calls;
  window.__errors = errors;
  window.__callbacks = callbacks;
  window.__fire = function (name) {
    const args = Array.prototype.slice.call(arguments, 1);
    const list = callbacks[name] || [];
    let fired = 0;
    for (const callback of list) {
      try {
        callback.apply(null, args);
        fired++;
      } catch (error) {
        errors.push('fire ' + name + ': ' + (error && error.stack ? error.stack : error));
      }
    }
    return { name, listeners: list.length, fired };
  };
  window.addEventListener('error', e => errors.push('error: ' + e.message + ' @ ' + e.filename + ':' + e.lineno));
  window.addEventListener('unhandledrejection', e => errors.push('rejection: ' + (e.reason && e.reason.stack ? e.reason.stack : e.reason)));
  const consoleError = console.error;
  console.error = function () {
    errors.push(
      'console.error: ' +
        Array.prototype.map
          .call(arguments, a => (a && a.stack ? a.stack : typeof a === 'object' ? JSON.stringify(a) : String(a)))
          .join(' ')
          .slice(0, 1500),
    );
    consoleError.apply(console, arguments);
  };
  window.skyrimtogether = new Proxy(
    {
      on(name, callback) {
        (callbacks[name] = callbacks[name] || []).push(callback);
      },
      off(name) {
        delete callbacks[name];
      },
    },
    {
      get(target, property) {
        if (property in target) return target[property];
        if (typeof property !== 'string') return undefined;
        return function () {
          calls.push(property + '(' + Array.prototype.map.call(arguments, a => JSON.stringify(a)).join(', ') + ')');
        };
      },
    },
  );
})();
