(ns knowledge.world.adapters.container
  "Container runtime adapter (spec 5.10 binds :container, 11.1): a Workspace appliance backed by
   a container. Not built yet; `start!` is the seam.")

(defn start! [_adapter _workspace]
  (throw (ex-info "container adapter not implemented" {:code :not-implemented :adapter :container})))
