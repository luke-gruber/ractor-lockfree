# frozen_string_literal: true

require_relative "lockfree/version"
require_relative "shared_var"

class Ractor
  # One shared variable, read and written without a lock.
  #
  #   Ractor::SharedVar   a typed, shareable value that any Ractor may replace
  #
  # Require this file, or the class on its own:
  #
  #   require "ractor/shared_var"
  module Lockfree
  end
end
