using SciMLTesting, DragonflyReverb

# Docs env does not depend on this sublibrary yet, so rendering is unchecked.
run_qa(DragonflyReverb; api_docs_kwargs = (; rendered = false))
