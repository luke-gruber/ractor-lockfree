# frozen_string_literal: true

require_relative "lockfree/version"
require_relative "shared_var"
require_relative "lockfree/hash"

class Ractor
  # Data structures Ractors share without a lock.
  #
  #   Ractor::SharedVar         one typed, shareable value any Ractor may replace
  #   Ractor::LockFree::Hash    a shareable key/value table any Ractor may write
  #
  # Require this file, or either structure on its own:
  #
  #   require "ractor/shared_var"
  #   require "ractor/lockfree/hash"
  module LockFree
  end
end
